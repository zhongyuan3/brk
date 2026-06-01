#include <brk/asm.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/refcnt.h>
#include <brk/signal.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
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

static bool user_access_ok(u64 addr, usize_t len)
{
	return addr + len <= USER_SPACE_SIZE_MAX;
}

void sigaction_table_reset(struct sigaction_table *table)
{
	spinlock_acquire(&table->lock);
	for (int i = 0; i < NSIG; ++i) {
		table->actions[i].sa_handler = (unsigned long)SIG_DFL;
		table->actions[i].sa_mask = 0;
		table->actions[i].sa_flags = 0;
	}
	spinlock_release(&table->lock);
}

static int next_signal(struct task_control_block *task)
{
	u64 deliver = task->pending & ~task->blocked;

	if (task->in_handler) {
		u64 nodfer = 0;
		int sig;

		deliver &= (1ULL << SIGKILL);
		spinlock_acquire(&task->sigactions->lock);
		for (sig = 1; sig < NSIG; ++sig) {
			if (!(task->pending & (1ULL << sig)))
				continue;
			if (task->sigactions->actions[sig].sa_flags &
			    SA_NODEFER)
				nodfer |= (1ULL << sig);
		}
		spinlock_release(&task->sigactions->lock);
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

void signal_reset(struct task_control_block *task)
{
	task->pending = 0;
	task->blocked = 0;
	task->in_handler = false;
	task->sigframe_sp = 0;
	sigaction_table_reset(task->sigactions);
}

static void clear_pending(struct task_control_block *task, int sig)
{
	task->pending &= ~(1ULL << sig);
}

bool signal_pending(struct task_control_block *task)
{
	bool pending;

	spinlock_acquire(&task->lock);
	pending = next_signal(task) != 0;
	spinlock_release(&task->lock);
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

	if (sig == 0) {
		spinlock_release(&target->lock);
		return 0;
	}

	target->pending |= (1ULL << sig);
	spinlock_release(&target->lock);

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
	task->pending |= (1ULL << sig);
	spinlock_release(&task->lock);

	task_wake_spec(task);
}

int signal_init(struct task_control_block *task)
{
	task->sigactions = sigaction_table_alloc();
	if (!task->sigactions)
		return -ENOMEM;
	signal_reset(task);
	return 0;
}

void signal_deinit(struct task_control_block *task)
{
	sigaction_table_put(task->sigactions);
}

static bool setup_signal_frame(struct task_control_block *task, int sig,
			       u64 handler)
{
	u64 sp;
	struct user_sigframe *frame;

	sp = task->tf->sp;
	sp -= sizeof(*frame);
	sp &= ~0xFULL;
	if (!user_access_ok(sp, sizeof(*frame)))
		return false;

	frame = (struct user_sigframe *)sp;
	frame->tf = *task->tf;
	frame->blocked = task->blocked;
	frame->signo = sig;

	task->sigframe_sp = sp;
	spinlock_acquire(&task->sigactions->lock);
	if (!(task->sigactions->actions[sig].sa_flags & SA_NODEFER))
		task->blocked |= (1ULL << sig);
	task->blocked |= task->sigactions->actions[sig].sa_mask;
	spinlock_release(&task->sigactions->lock);
	clear_pending(task, sig);
	task->in_handler = true;

	task->tf->sp = sp;
	task->tf->epc = handler;
	task->tf->a0 = sig;
	return true;
}

static void signal_deliver_one(struct task_control_block *task, int sig)
{
	unsigned long handler;
	u64 user_handler;

	spinlock_acquire(&task->lock);
	spinlock_acquire(&task->sigactions->lock);
	handler = task->sigactions->actions[sig].sa_handler;
	spinlock_release(&task->sigactions->lock);

	if (sig == SIGKILL ||
	    (handler == (unsigned long)SIG_DFL && sig_default_terminate(sig))) {
		spinlock_release(&task->lock);
		task_exit_signal(sig);
	}

	if (handler == (unsigned long)SIG_IGN) {
		clear_pending(task, sig);
		spinlock_release(&task->lock);
		return;
	}

	if (handler == (unsigned long)SIG_DFL) {
		clear_pending(task, sig);
		spinlock_release(&task->lock);
		return;
	}

	user_handler = handler;
	if (!setup_signal_frame(task, sig, user_handler)) {
		spinlock_release(&task->lock);
		task_exit_signal(sig);
	}
	spinlock_release(&task->lock);
}

void signal_deliver_pending(struct task_control_block *task)
{
	int sig;

	for (;;) {
		spinlock_acquire(&task->lock);
		sig = next_signal(task);
		spinlock_release(&task->lock);
		if (!sig)
			return;
		signal_deliver_one(task, sig);
		if (task->in_handler)
			return;
	}
}

void signal_copy(struct task_control_block *dst, struct task_control_block *src)
{
	dst->pending = 0;
	dst->blocked = src->blocked;
	dst->in_handler = false;
	dst->sigframe_sp = 0;
	sigaction_table_copy(dst->sigactions, src->sigactions);
}

u64 signal_do_sigreturn(struct task_control_block *task)
{
	struct user_sigframe *frame;

	if (!task->sigframe_sp)
		return -EINVAL;

	frame = (struct user_sigframe *)task->sigframe_sp;
	if (!user_access_ok(task->sigframe_sp, sizeof(*frame)))
		return -EFAULT;

	*task->tf = frame->tf;
	task->blocked = frame->blocked;
	task->in_handler = false;
	task->sigframe_sp = 0;
	return task->tf->a0;
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

	spinlock_acquire(&task->lock);
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
	spinlock_release(&task->lock);
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
	memcpy(dst->actions, src->actions, sizeof(dst->actions));
	spinlock_release(&dst->lock);
	spinlock_release(&src->lock);
}
