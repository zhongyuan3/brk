#include <brk/asm.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/process.h>
#include <brk/signal.h>
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

void proc_signal_init(struct process *proc)
{
	proc->pending = 0;
	proc->blocked = 0;
	proc->in_handler = false;
	proc->sigframe_sp = 0;
	for (int i = 0; i < NSIG; ++i) {
		proc->actions[i].sa_handler = (unsigned long)SIG_DFL;
		proc->actions[i].sa_mask = 0;
		proc->actions[i].sa_flags = 0;
	}
}

void proc_signal_fork(struct process *child, struct process *parent)
{
	child->pending = 0;
	child->blocked = parent->blocked;
	child->in_handler = false;
	child->sigframe_sp = 0;
	for (int i = 0; i < NSIG; ++i)
		child->actions[i] = parent->actions[i];
}

static int proc_next_signal(struct process *proc)
{
	u64 deliver = proc->pending & ~proc->blocked;

	if (proc->in_handler)
		deliver &= (1ULL << SIGKILL);

	if (!deliver)
		return 0;

	for (int sig = 1; sig < NSIG; ++sig) {
		if (deliver & (1ULL << sig))
			return sig;
	}
	return 0;
}

static void proc_clear_pending(struct process *proc, int sig)
{
	proc->pending &= ~(1ULL << sig);
}

bool proc_signal_pending(struct process *proc)
{
	bool pending;

	spinlock_acquire(&proc->lock);
	pending = proc_next_signal(proc) != 0;
	spinlock_release(&proc->lock);
	return pending;
}

int proc_kill(pid_t pid, int sig)
{
	struct process *p, *target = NULL;

	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;

	spinlock_acquire(&procs_lock);
	list_for_each_entry(p, &procs, list) {
		if (p->pid == pid) {
			target = p;
			break;
		}
	}
	if (!target) {
		spinlock_release(&procs_lock);
		return -ESRCH;
	}
	if (target == init_proc) {
		spinlock_release(&procs_lock);
		return -EPERM;
	}

	spinlock_acquire(&target->lock);
	spinlock_release(&procs_lock);

	if (target->state == PROCESS_STATE_ZOMBIE) {
		spinlock_release(&target->lock);
		return -ESRCH;
	}

	if (sig == 0) {
		spinlock_release(&target->lock);
		return 0;
	}

	target->pending |= (1ULL << sig);
	spinlock_release(&target->lock);

	proc_wake_process(target);
	return 0;
}

void proc_send_signal(struct process *proc, int sig)
{
	if (!proc || !sig_valid(sig))
		return;

	spinlock_acquire(&proc->lock);
	if (proc->state == PROCESS_STATE_ZOMBIE) {
		spinlock_release(&proc->lock);
		return;
	}
	proc->pending |= (1ULL << sig);
	spinlock_release(&proc->lock);

	proc_wake_process(proc);
}

static bool proc_setup_signal_frame(struct process *proc, int sig, u64 handler)
{
	u64 sp;
	struct user_sigframe *frame;

	sp = proc->tf.sp;
	sp -= sizeof(*frame);
	sp &= ~0xFULL;
	if (!user_access_ok(sp, sizeof(*frame)))
		return false;

	frame = (struct user_sigframe *)sp;
	frame->tf = proc->tf;
	frame->blocked = proc->blocked;
	frame->signo = sig;

	proc->sigframe_sp = sp;
	proc->blocked |= (1ULL << sig) | proc->actions[sig].sa_mask;
	proc_clear_pending(proc, sig);
	proc->in_handler = true;

	proc->tf.sp = sp;
	proc->tf.epc = handler;
	proc->tf.a0 = sig;
	return true;
}

static void proc_deliver_one(struct process *proc, int sig)
{
	unsigned long handler;
	u64 user_handler;

	spinlock_acquire(&proc->lock);
	handler = proc->actions[sig].sa_handler;

	if (sig == SIGKILL ||
	    (handler == (unsigned long)SIG_DFL && sig_default_terminate(sig))) {
		spinlock_release(&proc->lock);
		proc_exit_signal(sig);
	}

	if (handler == (unsigned long)SIG_IGN) {
		proc_clear_pending(proc, sig);
		spinlock_release(&proc->lock);
		return;
	}

	if (handler == (unsigned long)SIG_DFL) {
		proc_clear_pending(proc, sig);
		spinlock_release(&proc->lock);
		return;
	}

	user_handler = handler;
	if (!proc_setup_signal_frame(proc, sig, user_handler)) {
		spinlock_release(&proc->lock);
		proc_exit_signal(sig);
	}
	spinlock_release(&proc->lock);
}

void proc_deliver_pending(struct process *proc)
{
	int sig;

	for (;;) {
		spinlock_acquire(&proc->lock);
		sig = proc_next_signal(proc);
		spinlock_release(&proc->lock);
		if (!sig)
			return;
		proc_deliver_one(proc, sig);
		if (proc->in_handler)
			return;
	}
}

u64 proc_do_sigreturn(struct process *proc)
{
	struct user_sigframe *frame;

	if (!proc->sigframe_sp)
		return -EINVAL;

	frame = (struct user_sigframe *)proc->sigframe_sp;
	if (!user_access_ok(proc->sigframe_sp, sizeof(*frame)))
		return -EFAULT;

	proc->tf = frame->tf;
	proc->blocked = frame->blocked;
	proc->in_handler = false;
	proc->sigframe_sp = 0;
	return proc->tf.a0;
}

int proc_sigaction(struct process *proc, int sig, const struct sigaction *act,
		   struct sigaction *oact)
{
	if (!sig_valid(sig))
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;

	spinlock_acquire(&proc->lock);
	if (oact)
		*oact = proc->actions[sig];
	if (act)
		proc->actions[sig] = *act;
	spinlock_release(&proc->lock);
	return 0;
}

int proc_sigprocmask(struct process *proc, int how, const sigset_t *set,
		     sigset_t *oldset)
{
	sigset_t newblocked;

	spinlock_acquire(&proc->lock);
	if (oldset)
		*oldset = proc->blocked;

	if (set) {
		newblocked = proc->blocked;
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
			spinlock_release(&proc->lock);
			return -EINVAL;
		}
		newblocked &= ~(1ULL << SIGKILL);
		newblocked &= ~(1ULL << SIGSTOP);
		proc->blocked = newblocked;
	}
	spinlock_release(&proc->lock);
	return 0;
}
