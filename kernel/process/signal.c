#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/process.h>
#include <brk/signal.h>
#include <uapi/brk/errno.h>
#include <uapi/signal.h>

int proc_kill(pid_t pid, int sig)
{
	struct process *p, *target = NULL;

	if (sig != 0 && (sig < 0 || sig >= NSIG))
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

	if (sig == SIGKILL || !target->pending_sig)
		target->pending_sig = sig;
	spinlock_release(&target->lock);

	proc_wake_process(target);
	return 0;
}

void proc_send_signal(struct process *proc, int sig)
{
	if (!proc || sig <= 0 || sig >= NSIG)
		return;

	spinlock_acquire(&proc->lock);
	if (proc->state == PROCESS_STATE_ZOMBIE) {
		spinlock_release(&proc->lock);
		return;
	}
	if (sig == SIGKILL || !proc->pending_sig)
		proc->pending_sig = sig;
	spinlock_release(&proc->lock);

	proc_wake_process(proc);
}

void proc_deliver_fatal(struct process *proc)
{
	int sig;

	spinlock_acquire(&proc->lock);
	sig = proc->pending_sig;
	spinlock_release(&proc->lock);
	if (sig)
		proc_exit_signal(sig);
}
