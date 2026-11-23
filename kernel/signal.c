#include <arch/asm/vmlayout.h>
#include <arch/trapframe.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/permission.h>
#include <brk/process.h>
#include <brk/refcnt.h>
#include <brk/signal.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/thread.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <stdint.h>
#include <uapi/errno.h>
#include <uapi/signal.h>

static bool sig_valid(int sig)
{
	return sig > 0 && sig < NSIG;
}

static bool signal_may_send(struct process *target)
{
	struct process *current = current_process();

	if (capable(CAP_KILL))
		return true;
	return process_get_euid(target) == process_get_euid(current);
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

static bool user_access_ok(user_addr_t addr, size_t len)
{
	return addr + len <= USER_SPACE_SIZE_MAX;
}

static void sigaction_table_reset_locked(struct __brk_sigaction_table *table)
{
	for (int i = 0; i < NSIG; ++i) {
		table->actions[i].sa_handler = SIG_DFL;
		table->actions[i].sa_mask = 0;
		table->actions[i].sa_flags = 0;
	}
}

void sigaction_table_reset(struct __brk_sigaction_table *table)
{
	irqstate_t flags = spin_lock_irqsave(&table->lock);

	sigaction_table_reset_locked(table);
	spin_unlock_irqrestore(&table->lock, flags);
}

static u64 thread_pending_locked(struct thread *thread)
{
	return thread->pending | thread->process->shared_pending;
}

static u64 thread_deliverable_locked(struct thread *thread)
{
	u64 deliver = thread_pending_locked(thread) & ~thread->blocked;

	if (thread->in_handler) {
		u64 nodfer = 0;
		int sig;

		deliver &= (1ULL << SIGKILL);
		for (sig = 1; sig < NSIG; ++sig) {
			if (!(thread_pending_locked(thread) & (1ULL << sig)))
				continue;
			if (thread->process->sigactions.actions[sig].sa_flags &
			    SA_NODEFER)
				nodfer |= (1ULL << sig);
		}
		deliver |= nodfer & ~thread->blocked;
	}

	return deliver;
}

static int get_next_signal_locked(struct thread *thread)
{
	u64 deliver = thread_deliverable_locked(thread);

	if (!deliver)
		return 0;

	for (int sig = 1; sig < NSIG; ++sig) {
		if (deliver & (1ULL << sig))
			return sig;
	}
	return 0;
}

/*
 * A signal that will be discarded at delivery time (SIG_IGN, or a default
 * action that ignores it) must not abort a blocking syscall.  Otherwise a
 * pending SIGWINCH or SIGCHLD would make every wait return -EINTR even though
 * returning to user space would simply drop the signal.
 */
static bool signal_is_actionable_locked(struct process *process, int sig)
{
	void (*handler)(int);

	if (sig == SIGKILL)
		return true;

	handler = process->sigactions.actions[sig].sa_handler;
	if (handler == SIG_IGN)
		return false;
	if (handler == SIG_DFL && !sig_default_terminate(sig))
		return false;
	return true;
}

static void signal_reset_signal_sets_locked(struct thread *thread)
{
	thread->pending = 0;
	thread->blocked = 0;
	thread->in_handler = false;
	thread->sigframe_sp = 0;
}

static void signal_reset_locked(struct process *process, struct thread *thread)
{
	signal_reset_signal_sets_locked(thread);
	process->shared_pending = 0;
	sigaction_table_reset_locked(&process->sigactions);
}

void signal_reset(struct process *process, struct thread *thread)
{
	irqstate_t flags = spin_lock_irqsave(&process->sigactions.lock);

	signal_reset_locked(process, thread);
	spin_unlock_irqrestore(&process->sigactions.lock, flags);
}

static void clear_pending_locked(struct thread *thread, int sig)
{
	thread->pending &= ~(1ULL << sig);
	thread->process->shared_pending &= ~(1ULL << sig);
}

bool signal_pending(struct thread *thread)
{
	bool pending = false;
	u64 deliver;
	irqstate_t flags;

	if (!thread || !thread->process)
		return false;

	flags = spin_lock_irqsave(&thread->process->sigactions.lock);
	deliver = thread_deliverable_locked(thread);
	for (int sig = 1; sig < NSIG; ++sig) {
		if (!(deliver & (1ULL << sig)))
			continue;
		if (!signal_is_actionable_locked(thread->process, sig))
			continue;
		pending = true;
		break;
	}
	spin_unlock_irqrestore(&thread->process->sigactions.lock, flags);
	return pending;
}

static int signal_kill_pgrp(pid_t pgrp, int sig)
{
	struct process *process;
	bool found = false;
	bool sent = false;

	if (pgrp <= 0)
		return -ESRCH;
	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;

	irqstate_t flags = spin_lock_irqsave(&process_list_lock);
	list_for_each_entry(process, &processes, list) {
		if (process->pgid != pgrp)
			continue;
		found = true;
		if (!signal_may_send(process))
			continue;
		if (sig)
			signal_send_process(process, sig);
		sent = true;
	}
	spin_unlock_irqrestore(&process_list_lock, flags);

	if (sent)
		return 0;
	return found ? -EPERM : -ESRCH;
}

void signal_send_pgrp(pid_t pgrp, int sig)
{
	(void)signal_kill_pgrp(pgrp, sig);
}

int signal_do_kill(pid_t pid, int sig)
{
	struct process *process, *target = NULL;

	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;

	if (pid <= 0) {
		if (pid == -1) {
			bool found = false;
			bool sent = false;

			irqstate_t flags =
				spin_lock_irqsave(&process_list_lock);
			list_for_each_entry(process, &processes, list) {
				found = true;
				if (!signal_may_send(process))
					continue;
				if (sig)
					signal_send_process(process, sig);
				sent = true;
			}
			spin_unlock_irqrestore(&process_list_lock, flags);
			if (sent)
				return 0;
			return found ? -EPERM : -ESRCH;
		}

		return signal_kill_pgrp(
			pid == 0 ? current_process()->pgid : -pid, sig);
	}

	irqstate_t flags = spin_lock_irqsave(&process_list_lock);
	list_for_each_entry(process, &processes, list) {
		if (process->pid == pid) {
			target = process;
			break;
		}
	}
	if (!target) {
		spin_unlock_irqrestore(&process_list_lock, flags);
		return -ESRCH;
	}
	if (!signal_may_send(target)) {
		spin_unlock_irqrestore(&process_list_lock, flags);
		return -EPERM;
	}

	spin_lock(&target->lock);
	bool exited = target->exited;
	spin_unlock(&target->lock);

	if (exited) {
		spin_unlock_irqrestore(&process_list_lock, flags);
		return -ESRCH;
	}

	/*
	 * Keep process_list_lock while publishing the signal, so @target cannot
	 * be reaped underneath us.
	 */
	if (sig)
		signal_send_process(target, sig);
	spin_unlock_irqrestore(&process_list_lock, flags);

	return 0;
}

void signal_send(struct thread *thread, int sig)
{
	irqstate_t tflags, sflags;

	if (!thread || !sig_valid(sig))
		return;

	tflags = spin_lock_irqsave(&thread->lock);
	if (thread->state == THREAD_STATE_ZOMBIE) {
		spin_unlock_irqrestore(&thread->lock, tflags);
		return;
	}
	sflags = spin_lock_irqsave(&thread->process->sigactions.lock);
	spin_unlock_irqrestore(&thread->lock, tflags);
	thread->pending |= (1ULL << sig);
	spin_unlock_irqrestore(&thread->process->sigactions.lock, sflags);

	thread_wake(thread);
}

static struct thread *process_pick_live_thread_locked(struct process *process,
						      int sig)
{
	struct thread *thread;
	struct thread *fallback = NULL;

	list_for_each_entry(thread, &process->threads, proc_threads) {
		u64 blocked;

		if (thread->state == THREAD_STATE_ZOMBIE)
			continue;
		blocked = __atomic_load_n(&thread->blocked, __ATOMIC_RELAXED);
		if (!(blocked & (1ULL << sig)))
			return thread;
		if (!fallback)
			fallback = thread;
	}
	return fallback;
}

void signal_send_process(struct process *process, int sig)
{
	struct thread *wake = NULL;
	irqstate_t flags, sflags;

	if (!process || !sig_valid(sig))
		return;

	flags = spin_lock_irqsave(&process->lock);
	if (process->exited) {
		spin_unlock_irqrestore(&process->lock, flags);
		return;
	}
	wake = process_pick_live_thread_locked(process, sig);
	sflags = spin_lock_irqsave(&process->sigactions.lock);
	spin_unlock_irqrestore(&process->lock, flags);
	process->shared_pending |= (1ULL << sig);
	spin_unlock_irqrestore(&process->sigactions.lock, sflags);

	if (wake)
		thread_wake(wake);
}

void signal_thread_init(struct process *process, struct thread *thread,
			u64 blocked)
{
	irqstate_t flags;

	flags = spin_lock_irqsave(&process->sigactions.lock);
	signal_reset_signal_sets_locked(thread);
	thread->blocked = blocked;
	spin_unlock_irqrestore(&process->sigactions.lock, flags);
}

void signal_init(struct process *process, struct thread *thread, u64 blocked)
{
	sigaction_table_init(&process->sigactions);
	signal_thread_init(process, thread, blocked);
}

static bool setup_signal_frame_locked(struct thread *thread, int sig,
				      u64 handler)
{
	struct process *process = thread->process;
	user_addr_t sp;
	struct user_sigframe *frame;

	sp = trap_frame_get_sp(thread->frame);
	sp -= sizeof(*frame);
	sp &= ~0xFULL;
	if (!user_access_ok(sp, sizeof(*frame)))
		return false;

	frame = (struct user_sigframe *)(uintptr_t)sp;
	memcpy(&frame->tf, thread->frame, TRAP_FRAME_SIZE);
	frame->blocked = thread->blocked;
	frame->signo = sig;

	thread->sigframe_sp = sp;
	if (!(process->sigactions.actions[sig].sa_flags & SA_NODEFER))
		thread->blocked |= (1ULL << sig);
	thread->blocked |= process->sigactions.actions[sig].sa_mask;
	clear_pending_locked(thread, sig);
	thread->in_handler = true;

	trap_frame_set_sp(thread->frame, sp);
	trap_frame_set_pc(thread->frame, handler);
	trap_frame_set_ret_val(thread->frame, sig);
	return true;
}

static void signal_deliver_one(struct thread *thread, int sig)
{
	struct process *process = thread->process;
	void (*handler)(int);
	u64 user_handler;
	irqstate_t flags = spin_lock_irqsave(&process->sigactions.lock);

	handler = process->sigactions.actions[sig].sa_handler;

	if (sig == SIGKILL ||
	    (handler == SIG_DFL && sig_default_terminate(sig))) {
		spin_unlock_irqrestore(&process->sigactions.lock, flags);
		thread_exit_signal(sig);
		return;
	}

	if (handler == SIG_IGN) {
		clear_pending_locked(thread, sig);
		spin_unlock_irqrestore(&process->sigactions.lock, flags);
		return;
	}

	if (handler == SIG_DFL) {
		clear_pending_locked(thread, sig);
		spin_unlock_irqrestore(&process->sigactions.lock, flags);
		return;
	}

	user_handler = (u64)(uintptr_t)handler;
	if (!setup_signal_frame_locked(thread, sig, user_handler)) {
		spin_unlock_irqrestore(&process->sigactions.lock, flags);
		thread_exit_signal(sig);
		return;
	}
	spin_unlock_irqrestore(&process->sigactions.lock, flags);
}

void signal_deliver_pending(struct thread *thread)
{
	int sig;
	bool in_handler;

	for (;;) {
		irqstate_t flags =
			spin_lock_irqsave(&thread->process->sigactions.lock);

		sig = get_next_signal_locked(thread);
		spin_unlock_irqrestore(&thread->process->sigactions.lock,
				       flags);
		if (!sig)
			return;
		signal_deliver_one(thread, sig);
		flags = spin_lock_irqsave(&thread->process->sigactions.lock);
		in_handler = thread->in_handler;
		spin_unlock_irqrestore(&thread->process->sigactions.lock,
				       flags);
		if (in_handler)
			return;
	}
}

static void sigaction_table_copy_locked(struct __brk_sigaction_table *dst,
					struct __brk_sigaction_table *src)
{
	memcpy(dst->actions, src->actions, sizeof(dst->actions));
}

void signal_copy(struct process *dst_process, struct thread *dst_thread,
		 struct process *src_process, struct thread *src_thread)
{
	irqstate_t sflags = spin_lock_irqsave(&src_process->sigactions.lock);
	irqstate_t dflags = spin_lock_irqsave(&dst_process->sigactions.lock);

	dst_thread->pending = 0;
	dst_thread->blocked = src_thread->blocked;
	dst_thread->in_handler = false;
	dst_thread->sigframe_sp = 0;
	dst_process->shared_pending = 0;
	sigaction_table_copy_locked(&dst_process->sigactions,
				    &src_process->sigactions);
	spin_unlock_irqrestore(&dst_process->sigactions.lock, dflags);
	spin_unlock_irqrestore(&src_process->sigactions.lock, sflags);
}

u64 signal_do_sigreturn(struct thread *thread)
{
	struct user_sigframe *frame;
	u64 ret = 0;
	irqstate_t flags = spin_lock_irqsave(&thread->process->sigactions.lock);

	if (!thread->sigframe_sp) {
		ret = -EINVAL;
		goto unlock_and_return;
	}

	frame = (struct user_sigframe *)(uintptr_t)thread->sigframe_sp;
	if (!user_access_ok(thread->sigframe_sp, sizeof(*frame))) {
		ret = -EFAULT;
		goto unlock_and_return;
	}

	memcpy(thread->frame, &frame->tf, TRAP_FRAME_SIZE);
	thread->blocked = frame->blocked;
	thread->in_handler = false;
	thread->sigframe_sp = 0;
	ret = trap_frame_get_arg(thread->frame, 0);

unlock_and_return:
	spin_unlock_irqrestore(&thread->process->sigactions.lock, flags);
	return ret;
}

int signal_do_sigaction(struct process *process, int sig,
			const struct __brk_sigaction *act,
			struct __brk_sigaction *oact)
{
	struct __brk_sigaction_table *table = &process->sigactions;

	if (!sig_valid(sig))
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;

	irqstate_t flags = spin_lock_irqsave(&table->lock);

	if (oact)
		*oact = table->actions[sig];
	if (act)
		table->actions[sig] = *act;
	spin_unlock_irqrestore(&table->lock, flags);
	return 0;
}

int signal_do_sigprocmask(struct thread *thread, int how, const sigset_t *set,
			  sigset_t *oldset)
{
	sigset_t newblocked;
	irqstate_t flags = spin_lock_irqsave(&thread->process->sigactions.lock);

	if (oldset)
		*oldset = thread->blocked;

	if (set) {
		newblocked = thread->blocked;
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
			spin_unlock_irqrestore(
				&thread->process->sigactions.lock, flags);
			return -EINVAL;
		}
		newblocked &= ~(1ULL << SIGKILL);
		newblocked &= ~(1ULL << SIGSTOP);
		thread->blocked = newblocked;
	}
	spin_unlock_irqrestore(&thread->process->sigactions.lock, flags);
	return 0;
}

void sigaction_table_init(struct __brk_sigaction_table *table)
{
	spinlock_init(&table->lock, "sigaction_table");
	sigaction_table_reset_locked(table);
}

void sigaction_table_copy(struct __brk_sigaction_table *dst,
			  struct __brk_sigaction_table *src)
{
	irqstate_t sflags = spin_lock_irqsave(&src->lock);
	irqstate_t dflags = spin_lock_irqsave(&dst->lock);

	sigaction_table_copy_locked(dst, src);
	spin_unlock_irqrestore(&dst->lock, dflags);
	spin_unlock_irqrestore(&src->lock, sflags);
}

u64 sys_set_signal(void)
{
	int sig = syscall_arg_int(0);
	const struct __brk_sigaction *act = syscall_arg_ptr(1);
	struct __brk_sigaction *oact = syscall_arg_ptr(2);

	return signal_do_sigaction(current_process(), sig, act, oact);
}

u64 sys_set_sigmask(void)
{
	int how = syscall_arg_int(0);
	const sigset_t *set = syscall_arg_ptr(1);
	sigset_t *oldset = syscall_arg_ptr(2);

	return signal_do_sigprocmask(current_thread(), how, set, oldset);
}

u64 sys_get_pending(void)
{
	struct thread *thread = current_thread();
	sigset_t *uset = syscall_arg_ptr(0);
	sigset_t set;
	irqstate_t flags;

	if (!uset)
		return -EFAULT;

	flags = spin_lock_irqsave(&thread->process->sigactions.lock);
	set = thread_pending_locked(thread) & thread->blocked;
	spin_unlock_irqrestore(&thread->process->sigactions.lock, flags);

	if (copy_to_user(uset, &set, sizeof(set)))
		return -EFAULT;
	return 0;
}

u64 sys_wait_signal(void)
{
	const sigset_t *uset = syscall_arg_ptr(0);
	const struct __brk_timespec *utimeout = syscall_arg_ptr(1);
	__brk_siginfo_t *uinfo = syscall_arg_ptr(2);
	int wflags = syscall_arg_int(3);
	struct thread *thread = current_thread();
	sigset_t set;
	struct __brk_timespec req;
	s64 deadline = 0;
	bool timed = false;
	irqstate_t flags;
	int sig;

	if (!uset)
		return -EFAULT;
	if (wflags & ~SIG_WAIT_SETMASK)
		return -EINVAL;
	if (copy_from_user(&set, uset, sizeof(set)))
		return -EFAULT;
	if (set & ~(((u64)1 << NSIG) - 1))
		return -EINVAL;

	if (wflags & SIG_WAIT_SETMASK) {
		/*
		 * sigsuspend mode: install @set (KILL/STOP stay
		 * unblockable) and sleep until a delivery aborts the
		 * thread.  The temporary mask stays installed while the
		 * handler runs; libc restores the caller's mask after
		 * the handler returns, and pause() uses the same path
		 * with the caller's own mask.
		 */
		flags = spin_lock_irqsave(&thread->process->sigactions.lock);
		set &= ~(1ULL << SIGKILL);
		set &= ~(1ULL << SIGSTOP);
		thread->blocked = set;
		spin_unlock_irqrestore(&thread->process->sigactions.lock,
				       flags);
		while (!thread_abort_pending(thread))
			ktime_nanosleep(NS_PER_MS, NULL);

		return -EINTR;
	}

	if (utimeout) {
		if (copy_from_user(&req, utimeout, sizeof(req)))
			return -EFAULT;
		if (req.tv_sec < 0 || req.tv_nsec < 0 ||
		    req.tv_nsec >= (__brk_long_t)NS_PER_SEC)
			return -EINVAL;
		deadline = (s64)ktime_get_mono_ns() + ktime_from_timespec(&req);
		timed = true;
	}

	for (;;) {
		flags = spin_lock_irqsave(&thread->process->sigactions.lock);
		for (sig = 1; sig < NSIG; sig++) {
			if ((set & (1ULL << sig)) &&
			    (thread_pending_locked(thread) & (1ULL << sig)))
				break;
		}

		if (sig < NSIG) {
			/*
			 * Validate the info destination before consuming the
			 * signal so an EFAULT does not lose it.
			 */
			if (uinfo && !user_range_ok((u64)(uintptr_t)uinfo,
						    sizeof(__brk_siginfo_t))) {
				spin_unlock_irqrestore(
					&thread->process->sigactions.lock,
					flags);
				return -EFAULT;
			}
			clear_pending_locked(thread, sig);
			spin_unlock_irqrestore(
				&thread->process->sigactions.lock, flags);
			break;
		}

		/*
		 * Deliver unrelated pending signals instead of consuming them.
		 * Return to the trap path so signal_deliver_pending() runs.
		 */
		if (thread_pending_locked(thread) & ~thread->blocked & ~set) {
			spin_unlock_irqrestore(
				&thread->process->sigactions.lock, flags);
			return -EINTR;
		}

		if (timed && (s64)ktime_get_mono_ns() >= deadline) {
			spin_unlock_irqrestore(
				&thread->process->sigactions.lock, flags);
			return -EAGAIN;
		}

		if (thread_abort_pending(thread)) {
			spin_unlock_irqrestore(
				&thread->process->sigactions.lock, flags);
			return -EINTR;
		}

		spin_unlock_irqrestore(&thread->process->sigactions.lock,
				       flags);
		(void)ktime_nanosleep(NS_PER_MS, NULL);
	}

	if (uinfo) {
		__brk_siginfo_t info = { 0 };

		info.si_signo = sig;
		if (copy_to_user(uinfo, &info, sizeof(info)))
			return -EFAULT;
	}

	return (u64)sig;
}

u64 sys_restore_context(void)
{
	return signal_do_sigreturn(current_thread());
}
