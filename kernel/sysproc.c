#include <arch/pgtable.h>
#include <brk/error.h>
#include <brk/kmalloc.h>
#include <brk/permission.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/signal.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/thread.h>
#include <brk/uaccess.h>
#include <brk/vmalloc.h>
#include <uapi/errno.h>
#include <uapi/proc.h>
#include <uapi/resource.h>
#include <uapi/time.h>

u64 sys_waitid(void)
{
	int idtype = syscall_arg_int(0);
	pid_t id = syscall_arg_int(1);
	__brk_siginfo_t *infop = syscall_arg_ptr(2);
	int options = syscall_arg_int(3);
	kuid_t uid = 0;
	int status = 0;
	pid_t pid;
	int ret;

	if (options & ~(WNOHANG | WNOWAIT | WEXITED | WSTOPPED | WCONTINUED))
		return -EINVAL;

	/* Job control is not implemented yet. */
	if (options & (WSTOPPED | WCONTINUED))
		return -EINVAL;

	switch (idtype) {
	case P_ALL:
		pid = -1;
		break;
	case P_PID:
		if (id <= 0)
			return -EINVAL;
		pid = id;
		break;
	case P_PGID:
	default:
		return -EINVAL;
	}

	ret = process_wait(pid, &status, options, NULL, &uid);
	if (ret < 0)
		return ret;

	if (infop) {
		__brk_siginfo_t si = { 0 };

		if (ret == 0) {
			si.si_signo = 0;
		} else {
			si.si_signo = SIGCHLD;
			si.si_pid = ret;
			si.si_uid = uid;
			if ((status & 0x7f) == 0) {
				si.si_code = CLD_EXITED;
				si.si_status = (status >> 8) & 0xff;
			} else {
				si.si_code = CLD_KILLED;
				si.si_status = status & 0x7f;
			}
		}
		if (copy_to_user(infop, &si, sizeof(si)))
			return -EFAULT;
	}

	return 0;
}

u64 sys_fork(void)
{
	return process_fork();
}

u64 sys_exit(void)
{
	int status = syscall_arg_raw(0);

	thread_exit_normal(status);
	return 0;
}

u64 sys_exit_group(void)
{
	int status = syscall_arg_raw(0);

	process_exit_group((status & 0xff) << 8);
	return 0;
}

u64 sys_thread_create(void)
{
	struct __brk_thread_create_args args;
	struct thread *thread;
	void *uargs = syscall_arg_ptr(0);

	if (!uargs ||
	    !user_range_ok((user_addr_t)(uintptr_t)uargs, sizeof(args)))
		return -EFAULT;
	if (copy_from_user(&args, uargs, sizeof(args)))
		return -EFAULT;

	if (args.flags)
		return -EINVAL;
	if (!args.stack || (args.stack & 0xF) || args.stack < 16 ||
	    args.stack > USER_SPACE_SIZE_MAX)
		return -EINVAL;
	if (!args.entry || (args.entry & 1) || !user_range_ok(args.entry, 4))
		return -EINVAL;
	if (args.tls > USER_SPACE_SIZE_MAX)
		return -EINVAL;
	if (args.clear_tid) {
		if ((args.clear_tid & (sizeof(int) - 1)) ||
		    !user_range_ok(args.clear_tid, sizeof(int)))
			return -EINVAL;
	}

	thread = thread_create_user(current_process(), args.stack, args.entry,
				    args.arg, args.tls,
				    (int *)(uintptr_t)args.clear_tid);
	if (IS_ERR(thread))
		return PTR_ERR(thread);
	return (u64)thread->tid;
}

u64 sys_kill(void)
{
	pid_t pid = syscall_arg_int(0);
	int sig = syscall_arg_int(1);

	return signal_do_kill(pid, sig);
}

u64 sys_sched_yield(void)
{
	thread_yield();
	return 0;
}

u64 sys_getproc(void)
{
	struct process *process = current_process();
	struct __brk_proc *ubuf = syscall_arg_ptr(0);
	struct __brk_proc proc;
	irqstate_t flags;

	flags = spin_lock_irqsave(&process->cred_lock);
	proc.pid = process->pid;
	proc.ppid = process->parent ? process->parent->pid : 0;
	proc.pgid = process->pgid;
	proc.sid = process->sid;
	proc.tid = current_thread()->tid;
	proc.uid = process->cred.uid;
	proc.euid = process->cred.euid;
	proc.suid = process->cred.suid;
	proc.gid = process->cred.gid;
	proc.egid = process->cred.egid;
	proc.sgid = process->cred.sgid;
	proc.ngroups = process->cred.ngroups;
	if (proc.ngroups > 0)
		memcpy(proc.groups, process->cred.groups,
		       sizeof(proc.groups[0]) * (size_t)proc.ngroups);
	spin_unlock_irqrestore(&process->cred_lock, flags);

	if (!ubuf)
		return -EFAULT;
	if (copy_to_user(ubuf, &proc, sizeof(proc)))
		return -EFAULT;
	return 0;
}

static bool cred_capable_locked(const struct cred *cred, int cap)
{
	if (cap < 0 || cap > CAP_LAST_CAP)
		return false;
	return (cred->cap_effective & (1ULL << cap)) != 0;
}

static void update_caps_after_setuid_locked(struct process *process,
					    kuid_t old_euid)
{
	if (old_euid != 0 && process->cred.euid == 0) {
		process->cred.cap_effective = CAP_FULL_SET;
		process->cred.cap_permitted = CAP_FULL_SET;
	} else if (old_euid == 0 && process->cred.euid != 0) {
		process->cred.cap_effective = 0;
		process->cred.cap_permitted = 0;
	}
}

/*
 * Credential slots: one setter per uid/gid slot (real, effective,
 * saved), the primitive ABI behind the POSIX setuid/setreuid/
 * setresuid family that libc composes.  The unprivileged rule is
 * uniform and is exactly what POSIX setresuid already allows: a
 * slot may move to any of the process's three current ids of that
 * kind.  CAP_SETUID/CAP_SETGID lifts all restrictions.  fsuid and
 * fsgid track the effective ids, and the capability sets are
 * refreshed when the effective uid crosses 0.
 */
static int uid_slot_ok(const struct cred *cred, int uid)
{
	return cred_capable_locked(cred, CAP_SETUID) ||
	       (kuid_t)uid == cred->uid ||
	       (kuid_t)uid == cred->euid ||
	       (kuid_t)uid == cred->suid;
}

static int gid_slot_ok(const struct cred *cred, int gid)
{
	return cred_capable_locked(cred, CAP_SETGID) ||
	       (kgid_t)gid == cred->gid ||
	       (kgid_t)gid == cred->egid ||
	       (kgid_t)gid == cred->sgid;
}

u64 sys_set_uid_real(void)
{
	struct process *process = current_process();
	int uid = syscall_arg_int(0);
	irqstate_t flags;
	int ret = 0;

	if (uid < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (!uid_slot_ok(&process->cred, uid))
		ret = -EPERM;
	else
		process->cred.uid = (kuid_t)uid;
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_set_gid_real(void)
{
	struct process *process = current_process();
	int gid = syscall_arg_int(0);
	irqstate_t flags;
	int ret = 0;

	if (gid < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (!gid_slot_ok(&process->cred, gid))
		ret = -EPERM;
	else
		process->cred.gid = (kgid_t)gid;
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_set_uid_effective(void)
{
	struct process *process = current_process();
	int uid = syscall_arg_int(0);
	irqstate_t flags;
	kuid_t old_euid;
	int ret = 0;

	if (uid < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	old_euid = process->cred.euid;
	if (!uid_slot_ok(&process->cred, uid)) {
		ret = -EPERM;
	} else {
		process->cred.euid = (kuid_t)uid;
		process->cred.fsuid = (kuid_t)uid;
		update_caps_after_setuid_locked(process, old_euid);
	}
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_set_gid_effective(void)
{
	struct process *process = current_process();
	int gid = syscall_arg_int(0);
	irqstate_t flags;
	int ret = 0;

	if (gid < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (!gid_slot_ok(&process->cred, gid))
		ret = -EPERM;
	else {
		process->cred.egid = (kgid_t)gid;
		process->cred.fsgid = (kgid_t)gid;
	}
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_set_uid_saved(void)
{
	struct process *process = current_process();
	int uid = syscall_arg_int(0);
	irqstate_t flags;
	int ret = 0;

	if (uid < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (!uid_slot_ok(&process->cred, uid))
		ret = -EPERM;
	else
		process->cred.suid = (kuid_t)uid;
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_set_gid_saved(void)
{
	struct process *process = current_process();
	int gid = syscall_arg_int(0);
	irqstate_t flags;
	int ret = 0;

	if (gid < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (!gid_slot_ok(&process->cred, gid))
		ret = -EPERM;
	else
		process->cred.sgid = (kgid_t)gid;
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_umask(void)
{
	struct process *process = current_process();
	umode_t old;
	irqstate_t flags = spin_lock_irqsave(&process->cred_lock);

	old = process->cred.umask;
	process->cred.umask = syscall_arg_raw(0) & 0777;
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return old;
}

u64 sys_setpgid(void)
{
	pid_t pid = syscall_arg_int(0);
	pid_t pgid = syscall_arg_int(1);
	struct process *process = current_process();

	if (pid != 0 && pid != process->pid)
		return -ESRCH;
	if (pgid < 0)
		return -EINVAL;
	if (pgid == 0)
		pgid = process->pid;

	/* A session leader cannot change its process group. */
	if (process->sid == process->pid)
		return -EPERM;
	if (pgid != process->pid && !process_pgrp_exists(pgid, process->sid))
		return -EPERM;

	process->pgid = pgid;
	return 0;
}

u64 sys_setsid(void)
{
	struct process *process = current_process();

	if (process->pgid == process->pid)
		return -EPERM;

	process->sid = process->pid;
	process->pgid = process->pid;
	return process->sid;
}

u64 sys_getpgid(void)
{
	pid_t pid = syscall_arg_int(0);

	if (pid == 0)
		return current_process()->pgid;
	return process_get_pgid(pid);
}

u64 sys_getsid(void)
{
	pid_t pid = syscall_arg_int(0);

	if (pid == 0)
		return current_process()->sid;
	return process_get_sid(pid);
}
