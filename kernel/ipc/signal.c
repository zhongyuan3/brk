#include <asm/vas_layout.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/refcnt.h>
#include <brk/signal.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/signal.h>

static bool sig_valid(int sig)
{
	return sig > 0 && sig < NSIG;
}

static bool sig_default_terminate(int sig)
{
	switch (sig) {
	case SIGCHLD:
	case SIGURG:
	case SIGWINCH:
	case SIGCONT:
		return false;
	default:
		return true;
	}
}

static bool user_access_ok(u64 addr, size_t len)
{
	return addr + len <= USER_SPACE_SIZE_MAX;
}

static void sigaction_table_reset_locked(struct sigaction_table *table)
{
	for (int i = 0; i < NSIG; ++i) {
		table->actions[i].sa_handler = (unsigned long)SIG_DFL;
		table->actions[i].sa_mask = 0;
		table->actions[i].sa_flags = 0;
	}
}

void sigaction_table_reset(struct sigaction_table *table)
{
	spinlock_acquire(&table->lock);
	sigaction_table_reset_locked(table);
	spinlock_release(&table->lock);
}

static int get_next_signal_locked(struct task_control_block *task)
{
	u64 deliver = task->pending & ~task->blocked;

	if (task->in_handler) {
		u64 nodfer = 0;
		int sig;

		deliver &= (1ULL << SIGKILL);
		for (sig = 1; sig < NSIG; ++sig) {
			if (!(task->pending & (1ULL << sig)))
				continue;
			if (task->sigactions->actions[sig].sa_flags &
			    SA_NODEFER)
				nodfer |= (1ULL << sig);
		}
		deliver |= nodfer & ~task->blocked;
	}

	if (!deliver)
		return 0;

	for (int sig = 1; sig < NSIG; ++sig) {
		if (deliver & (1ULL << sig))
			return sig;
	}
	return 0;
}

static void signal_reset_signal_sets_locked(struct task_control_block *task)
{
	task->pending = 0;
	task->blocked = 0;
	task->in_handler = false;
	task->sigframe_sp = 0;
}

static void signal_reset_locked(struct task_control_block *task)
{
	signal_reset_signal_sets_locked(task);
	sigaction_table_reset_locked(task->sigactions);
}

void signal_reset(struct task_control_block *task)
{
	spinlock_acquire(&task->sigactions->lock);
	signal_reset_locked(task);
	spinlock_release(&task->sigactions->lock);
}

static void clear_pending_locked(struct task_control_block *task, int sig)
{
	task->pending &= ~(1ULL << sig);
}

bool signal_pending(struct task_control_block *task)
{
	bool pending;

	spinlock_acquire(&task->sigactions->lock);
	pending = get_next_signal_locked(task) != 0;
	spinlock_release(&task->sigactions->lock);
	return pending;
}

int signal_do_kill(pid_t pid, int sig)
{
	struct task_control_block *p, *target = NULL;

	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;

	spinlock_acquire(&tasks_lock);
	list_for_each_entry(p, &tasks, list) {
		if (p->pid == pid) {
			target = p;
			break;
		}
	}
	if (!target) {
		spinlock_release(&tasks_lock);
		return -ESRCH;
	}
	if (target == initial_task) {
		spinlock_release(&tasks_lock);
		return -EPERM;
	}

	spinlock_acquire(&target->lock);
	spinlock_release(&tasks_lock);

	if (target->state == TASK_STATE_ZOMBIE) {
		spinlock_release(&target->lock);
		return -ESRCH;
	}

	spinlock_acquire(&target->sigactions->lock);
	spinlock_release(&target->lock);

	if (sig == 0) {
		spinlock_release(&target->sigactions->lock);
		return 0;
	}

	target->pending |= (1ULL << sig);
	spinlock_release(&target->sigactions->lock);

	task_wake_spec(target);
	return 0;
}

void signal_send(struct task_control_block *task, int sig)
{
	if (!task || !sig_valid(sig))
		return;

	spinlock_acquire(&task->lock);
	if (task->state == TASK_STATE_ZOMBIE) {
		spinlock_release(&task->lock);
		return;
	}
	spinlock_acquire(&task->sigactions->lock);
	spinlock_release(&task->lock);
	task->pending |= (1ULL << sig);
	spinlock_release(&task->sigactions->lock);

	task_wake_spec(task);
}

int signal_init(struct task_control_block *task,
		struct sigaction_table *sigactions, u64 blocked)
{
	if (!sigactions) {
		task->sigactions = sigaction_table_alloc();
		if (!task->sigactions)
			return -ENOMEM;
		signal_reset(task);
	} else {
		task->sigactions = sigaction_table_get(sigactions);
		spinlock_acquire(&task->sigactions->lock);
		signal_reset_signal_sets_locked(task);
		task->blocked = blocked;
		spinlock_release(&task->sigactions->lock);
	}
	return 0;
}

void signal_deinit(struct task_control_block *task)
{
	if (!task->sigactions)
		return;

	sigaction_table_put(task->sigactions);
}

static bool setup_signal_frame_locked(struct task_control_block *task, int sig,
				      u64 handler)
{
	u64 sp;
	struct user_sigframe *frame;

	sp = arch_tf_get_sp(task->tf);
	sp -= sizeof(*frame);
	sp &= ~0xFULL;
	if (!user_access_ok(sp, sizeof(*frame)))
		return false;

	frame = (struct user_sigframe *)sp;
	arch_tf_copy(&frame->tf, task->tf);
	frame->blocked = task->blocked;
	frame->signo = sig;

	task->sigframe_sp = sp;
	if (!(task->sigactions->actions[sig].sa_flags & SA_NODEFER))
		task->blocked |= (1ULL << sig);
	task->blocked |= task->sigactions->actions[sig].sa_mask;
	clear_pending_locked(task, sig);
	task->in_handler = true;

	arch_tf_set_sp(task->tf, sp);
	arch_tf_set_pc(task->tf, handler);
	arch_tf_set_a0(task->tf, sig);
	return true;
}

static void signal_deliver_one(struct task_control_block *task, int sig)
{
	unsigned long handler;
	u64 user_handler;

	spinlock_acquire(&task->sigactions->lock);

	handler = task->sigactions->actions[sig].sa_handler;

	if (sig == SIGKILL ||
	    (handler == (unsigned long)SIG_DFL && sig_default_terminate(sig))) {
		spinlock_release(&task->sigactions->lock);
		task_exit_signal(sig);
		return;
	}

	if (handler == (unsigned long)SIG_IGN) {
		clear_pending_locked(task, sig);
		spinlock_release(&task->sigactions->lock);
		return;
	}

	if (handler == (unsigned long)SIG_DFL) {
		clear_pending_locked(task, sig);
		spinlock_release(&task->sigactions->lock);
		return;
	}

	user_handler = handler;
	if (!setup_signal_frame_locked(task, sig, user_handler)) {
		spinlock_release(&task->sigactions->lock);
		task_exit_signal(sig);
		return;
	}
	spinlock_release(&task->sigactions->lock);
}

void signal_deliver_pending(struct task_control_block *task)
{
	int sig;
	bool in_handler;

	for (;;) {
		spinlock_acquire(&task->sigactions->lock);
		sig = get_next_signal_locked(task);
		spinlock_release(&task->sigactions->lock);
		if (!sig)
			return;
		signal_deliver_one(task, sig);
		spinlock_acquire(&task->sigactions->lock);
		in_handler = task->in_handler;
		spinlock_release(&task->sigactions->lock);
		if (in_handler)
			return;
	}
}

static void sigaction_table_copy_locked(struct sigaction_table *dst,
					struct sigaction_table *src)
{
	memcpy(dst->actions, src->actions, sizeof(dst->actions));
}

void signal_copy(struct task_control_block *dst, struct task_control_block *src)
{
	spinlock_acquire(&src->sigactions->lock);
	spinlock_acquire(&dst->sigactions->lock);
	dst->pending = 0;
	dst->blocked = src->blocked;
	dst->in_handler = false;
	dst->sigframe_sp = 0;
	sigaction_table_copy_locked(dst->sigactions, src->sigactions);
	spinlock_release(&dst->sigactions->lock);
	spinlock_release(&src->sigactions->lock);
}

u64 signal_do_sigreturn(struct task_control_block *task)
{
	struct user_sigframe *frame;
	u64 ret = 0;

	spinlock_acquire(&task->sigactions->lock);
	if (!task->sigframe_sp) {
		ret = -EINVAL;
		goto unlock_and_return;
	}

	frame = (struct user_sigframe *)task->sigframe_sp;
	if (!user_access_ok(task->sigframe_sp, sizeof(*frame))) {
		ret = -EFAULT;
		goto unlock_and_return;
	}

	arch_tf_copy(task->tf, &frame->tf);
	task->blocked = frame->blocked;
	task->in_handler = false;
	task->sigframe_sp = 0;
	ret = arch_tf_get_a0(task->tf);

unlock_and_return:
	spinlock_release(&task->sigactions->lock);
	return ret;
}

int signal_do_sigaction(struct task_control_block *task, int sig,
			const struct sigaction *act, struct sigaction *oact)
{
	struct sigaction_table *table = task->sigactions;

	if (!sig_valid(sig))
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;

	spinlock_acquire(&table->lock);
	if (oact)
		*oact = table->actions[sig];
	if (act)
		table->actions[sig] = *act;
	spinlock_release(&table->lock);
	return 0;
}

int signal_do_sigprocmask(struct task_control_block *task, int how,
			  const sigset_t *set, sigset_t *oldset)
{
	sigset_t newblocked;

	spinlock_acquire(&task->sigactions->lock);
	if (oldset)
		*oldset = task->blocked;

	if (set) {
		newblocked = task->blocked;
		switch (how) {
		case SIG_BLOCK:
			newblocked |= *set;
			break;
		case SIG_UNBLOCK:
			newblocked &= ~*set;
			break;
		case SIG_SETMASK:
			newblocked = *set;
			break;
		default:
			spinlock_release(&task->lock);
			return -EINVAL;
		}
		newblocked &= ~(1ULL << SIGKILL);
		newblocked &= ~(1ULL << SIGSTOP);
		task->blocked = newblocked;
	}
	spinlock_release(&task->sigactions->lock);
	return 0;
}

struct sigaction_table *sigaction_table_alloc(void)
{
	struct sigaction_table *table;

	table = kzalloc(sizeof(struct sigaction_table));
	if (!table)
		return NULL;

	refcnt_init(&table->refcnt, 1);
	spinlock_init(&table->lock, "sigaction_table");

	return table;
}

void sigaction_table_put(struct sigaction_table *table)
{
	if (refcnt_dec_fetch(&table->refcnt) > 0)
		return;

	kfree(table);
}

struct sigaction_table *sigaction_table_get(struct sigaction_table *table)
{
	refcnt_inc(&table->refcnt);
	return table;
}

void sigaction_table_copy(struct sigaction_table *dst,
			  struct sigaction_table *src)
{
	spinlock_acquire(&src->lock);
	spinlock_acquire(&dst->lock);
	sigaction_table_copy_locked(dst, src);
	spinlock_release(&dst->lock);
	spinlock_release(&src->lock);
}
