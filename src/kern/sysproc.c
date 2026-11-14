#include <arch/pgtable.h>
#include <brk/kmalloc.h>
#include <brk/error.h>
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
	siginfo_t *infop = syscall_arg_ptr(2);
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
		siginfo_t si = { 0 };

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

u64 sys_gettid(void)
{
	return current_thread()->tid;
}

u64 sys_thread_create(void)
{
	struct brk_thread_create_args args;
	struct thread *thread;
	void *uargs = syscall_arg_ptr(0);

	if (!uargs || !user_range_ok((u64)(uintptr_t)uargs, sizeof(args)))
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
	struct brk_proc *ubuf = syscall_arg_ptr(0);
	struct brk_proc proc;
	irqstate_t flags;

	flags = spin_lock_irqsave(&process->cred_lock);
	proc.pid = process->pid;
	proc.ppid = process->parent ? process->parent->pid : 0;
	proc.pgid = process->pgid;
	proc.sid = process->sid;
	proc.uid = process->cred.uid;
	proc.euid = process->cred.euid;
	proc.suid = process->cred.suid;
	proc.gid = process->cred.gid;
	proc.egid = process->cred.egid;
	proc.sgid = process->cred.sgid;
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

u64 sys_setuid(void)
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
	if (cred_capable_locked(&process->cred, CAP_SETUID)) {
		process->cred.uid = process->cred.euid = process->cred.suid =
			(kuid_t)uid;
		process->cred.fsuid = (kuid_t)uid;
	} else if ((kuid_t)uid == process->cred.uid ||
		   (kuid_t)uid == process->cred.suid) {
		process->cred.euid = (kuid_t)uid;
		process->cred.fsuid = (kuid_t)uid;
	} else {
		ret = -EPERM;
	}
	if (!ret)
		update_caps_after_setuid_locked(process, old_euid);
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_setgid(void)
{
	struct process *process = current_process();
	int gid = syscall_arg_int(0);
	irqstate_t flags;
	int ret = 0;

	if (gid < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (cred_capable_locked(&process->cred, CAP_SETGID)) {
		process->cred.gid = process->cred.egid = process->cred.sgid =
			(kgid_t)gid;
		process->cred.fsgid = (kgid_t)gid;
	} else if ((kgid_t)gid == process->cred.gid ||
		   (kgid_t)gid == process->cred.sgid) {
		process->cred.egid = (kgid_t)gid;
		process->cred.fsgid = (kgid_t)gid;
	} else {
		ret = -EPERM;
	}
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_setreuid(void)
{
	struct process *process = current_process();
	int ruid = syscall_arg_int(0);
	int euid = syscall_arg_int(1);
	irqstate_t flags;
	kuid_t old_euid;
	int ret = 0;

	if (ruid < -1 || euid < -1)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	old_euid = process->cred.euid;

	if (!cred_capable_locked(&process->cred, CAP_SETUID)) {
		if ((ruid != -1 && (kuid_t)ruid != process->cred.uid) ||
		    (euid != -1 && (kuid_t)euid != process->cred.uid &&
		     (kuid_t)euid != process->cred.suid)) {
			ret = -EPERM;
			goto out;
		}
	}

	if (ruid != -1)
		process->cred.uid = (kuid_t)ruid;
	if (euid != -1) {
		process->cred.euid = (kuid_t)euid;
		process->cred.fsuid = (kuid_t)euid;
	}
	update_caps_after_setuid_locked(process, old_euid);
out:
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_setregid(void)
{
	struct process *process = current_process();
	int rgid = syscall_arg_int(0);
	int egid = syscall_arg_int(1);
	irqstate_t flags;
	int ret = 0;

	if (rgid < -1 || egid < -1)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (!cred_capable_locked(&process->cred, CAP_SETGID)) {
		if ((rgid != -1 && (kgid_t)rgid != process->cred.gid) ||
		    (egid != -1 && (kgid_t)egid != process->cred.gid &&
		     (kgid_t)egid != process->cred.sgid)) {
			ret = -EPERM;
			goto out;
		}
	}

	if (rgid != -1)
		process->cred.gid = (kgid_t)rgid;
	if (egid != -1) {
		process->cred.egid = (kgid_t)egid;
		process->cred.fsgid = (kgid_t)egid;
	}
out:
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

static bool resuid_valid_locked(struct process *process, int ruid, int euid,
				int suid)
{
	if (cred_capable_locked(&process->cred, CAP_SETUID))
		return true;

	if (ruid != -1 && (kuid_t)ruid != process->cred.uid &&
	    (kuid_t)ruid != process->cred.euid &&
	    (kuid_t)ruid != process->cred.suid)
		return false;
	if (euid != -1 && (kuid_t)euid != process->cred.uid &&
	    (kuid_t)euid != process->cred.euid &&
	    (kuid_t)euid != process->cred.suid)
		return false;
	if (suid != -1 && (kuid_t)suid != process->cred.uid &&
	    (kuid_t)suid != process->cred.euid &&
	    (kuid_t)suid != process->cred.suid)
		return false;
	return true;
}

u64 sys_setresuid(void)
{
	struct process *process = current_process();
	int ruid = syscall_arg_int(0);
	int euid = syscall_arg_int(1);
	int suid = syscall_arg_int(2);
	irqstate_t flags;
	kuid_t old_euid;
	int ret = 0;

	if (ruid < -1 || euid < -1 || suid < -1)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	old_euid = process->cred.euid;
	if (!resuid_valid_locked(process, ruid, euid, suid)) {
		ret = -EPERM;
		goto out;
	}

	if (ruid != -1)
		process->cred.uid = (kuid_t)ruid;
	if (euid != -1)
		process->cred.euid = (kuid_t)euid;
	if (suid != -1)
		process->cred.suid = (kuid_t)suid;
	process->cred.fsuid = process->cred.euid;
	update_caps_after_setuid_locked(process, old_euid);
out:
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

static bool resgid_valid_locked(struct process *process, int rgid, int egid,
				int sgid)
{
	if (cred_capable_locked(&process->cred, CAP_SETGID))
		return true;

	if (rgid != -1 && (kgid_t)rgid != process->cred.gid &&
	    (kgid_t)rgid != process->cred.egid &&
	    (kgid_t)rgid != process->cred.sgid)
		return false;
	if (egid != -1 && (kgid_t)egid != process->cred.gid &&
	    (kgid_t)egid != process->cred.egid &&
	    (kgid_t)egid != process->cred.sgid)
		return false;
	if (sgid != -1 && (kgid_t)sgid != process->cred.gid &&
	    (kgid_t)sgid != process->cred.egid &&
	    (kgid_t)sgid != process->cred.sgid)
		return false;
	return true;
}

u64 sys_setresgid(void)
{
	struct process *process = current_process();
	int rgid = syscall_arg_int(0);
	int egid = syscall_arg_int(1);
	int sgid = syscall_arg_int(2);
	irqstate_t flags;
	int ret = 0;

	if (rgid < -1 || egid < -1 || sgid < -1)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	if (!resgid_valid_locked(process, rgid, egid, sgid)) {
		ret = -EPERM;
		goto out;
	}

	if (rgid != -1)
		process->cred.gid = (kgid_t)rgid;
	if (egid != -1)
		process->cred.egid = (kgid_t)egid;
	if (sgid != -1)
		process->cred.sgid = (kgid_t)sgid;
	process->cred.fsgid = process->cred.egid;
out:
	spin_unlock_irqrestore(&process->cred_lock, flags);
	return (u64)ret;
}

u64 sys_getgroups(void)
{
	struct process *process = current_process();
	int size = syscall_arg_int(0);
	u32 *list = syscall_arg_ptr(1);
	kgid_t groups[NGROUPS_MAX];
	int ngroups;
	irqstate_t flags;

	if (size < 0)
		return -EINVAL;

	flags = spin_lock_irqsave(&process->cred_lock);
	ngroups = process->cred.ngroups;
	if (ngroups > 0)
		memcpy(groups, process->cred.groups,
		       sizeof(groups[0]) * (size_t)ngroups);
	spin_unlock_irqrestore(&process->cred_lock, flags);

	if (size == 0)
		return ngroups;
	if (size < ngroups)
		return -EINVAL;
	if (!list)
		return -EFAULT;

	for (int i = 0; i < ngroups; i++) {
		u32 gid = groups[i];

		if (copy_to_user(&list[i], &gid, sizeof(gid)))
			return -EFAULT;
	}
	return ngroups;
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
