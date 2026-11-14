#include <arch/pgtable.h>
#include <brk/kmalloc.h>
#include <brk/permission.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/signal.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
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

	ret = task_wait(pid, &status, options, NULL, &uid);
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
	return task_fork();
}

u64 sys_exit(void)
{
	int status = syscall_arg_raw(0);

	task_exit_normal(status);
	return 0;
}

u64 sys_kill(void)
{
	pid_t pid = syscall_arg_int(0);
	int sig = syscall_arg_int(1);

	return signal_do_kill(pid, sig);
}

u64 sys_sched_yield(void)
{
	task_yield();
	return 0;
}

u64 sys_getproc(void)
{
	struct task *task = current_task();
	struct brk_proc *ubuf = syscall_arg_ptr(0);
	struct brk_proc proc = {
		.pid = task->tgid,
		.ppid = task->parent ? task->parent->tgid : 0,
		.pgid = task->pgid,
		.sid = task->sid,
		.uid = task->uid,
		.euid = task->euid,
		.suid = task->suid,
		.gid = task->gid,
		.egid = task->egid,
		.sgid = task->sgid,
	};

	if (!ubuf)
		return -EFAULT;
	if (copy_to_user(ubuf, &proc, sizeof(proc)))
		return -EFAULT;
	return 0;
}

static void update_caps_after_setuid(struct task *task, kuid_t old_euid)
{
	if (old_euid != 0 && task->euid == 0) {
		task->cap_effective = CAP_FULL_SET;
		task->cap_permitted = CAP_FULL_SET;
	} else if (old_euid == 0 && task->euid != 0) {
		task->cap_effective = 0;
		task->cap_permitted = 0;
	}
}

u64 sys_setuid(void)
{
	struct task *task = current_task();
	int uid = syscall_arg_int(0);
	kuid_t old_euid = task->euid;

	if (uid < 0)
		return -EINVAL;

	if (capable(CAP_SETUID)) {
		task->uid = task->euid = task->suid = (kuid_t)uid;
		task->fsuid = (kuid_t)uid;
	} else if ((kuid_t)uid == task->uid || (kuid_t)uid == task->suid) {
		task->euid = (kuid_t)uid;
		task->fsuid = (kuid_t)uid;
	} else {
		return -EPERM;
	}
	update_caps_after_setuid(task, old_euid);
	return 0;
}

u64 sys_setgid(void)
{
	struct task *task = current_task();
	int gid = syscall_arg_int(0);

	if (gid < 0)
		return -EINVAL;

	if (capable(CAP_SETGID)) {
		task->gid = task->egid = task->sgid = (kgid_t)gid;
		task->fsgid = (kgid_t)gid;
	} else if ((kgid_t)gid == task->gid || (kgid_t)gid == task->sgid) {
		task->egid = (kgid_t)gid;
		task->fsgid = (kgid_t)gid;
	} else {
		return -EPERM;
	}
	return 0;
}

u64 sys_setreuid(void)
{
	struct task *task = current_task();
	int ruid = syscall_arg_int(0);
	int euid = syscall_arg_int(1);
	kuid_t old_euid = task->euid;

	if (ruid < -1 || euid < -1)
		return -EINVAL;
	if (!capable(CAP_SETUID)) {
		if ((ruid != -1 && (kuid_t)ruid != task->uid) ||
		    (euid != -1 && (kuid_t)euid != task->uid &&
		     (kuid_t)euid != task->suid))
			return -EPERM;
	}

	if (ruid != -1)
		task->uid = (kuid_t)ruid;
	if (euid != -1) {
		task->euid = (kuid_t)euid;
		task->fsuid = (kuid_t)euid;
	}
	update_caps_after_setuid(task, old_euid);
	return 0;
}

u64 sys_setregid(void)
{
	struct task *task = current_task();
	int rgid = syscall_arg_int(0);
	int egid = syscall_arg_int(1);

	if (rgid < -1 || egid < -1)
		return -EINVAL;
	if (!capable(CAP_SETGID)) {
		if ((rgid != -1 && (kgid_t)rgid != task->gid) ||
		    (egid != -1 && (kgid_t)egid != task->gid &&
		     (kgid_t)egid != task->sgid))
			return -EPERM;
	}

	if (rgid != -1)
		task->gid = (kgid_t)rgid;
	if (egid != -1) {
		task->egid = (kgid_t)egid;
		task->fsgid = (kgid_t)egid;
	}
	return 0;
}

static bool resuid_valid(struct task *task, int ruid, int euid, int suid)
{
	if (capable(CAP_SETUID))
		return true;

	if (ruid != -1 && (kuid_t)ruid != task->uid &&
	    (kuid_t)ruid != task->euid && (kuid_t)ruid != task->suid)
		return false;
	if (euid != -1 && (kuid_t)euid != task->uid &&
	    (kuid_t)euid != task->euid && (kuid_t)euid != task->suid)
		return false;
	if (suid != -1 && (kuid_t)suid != task->uid &&
	    (kuid_t)suid != task->euid && (kuid_t)suid != task->suid)
		return false;
	return true;
}

u64 sys_setresuid(void)
{
	struct task *task = current_task();
	int ruid = syscall_arg_int(0);
	int euid = syscall_arg_int(1);
	int suid = syscall_arg_int(2);
	kuid_t old_euid = task->euid;

	if (ruid < -1 || euid < -1 || suid < -1)
		return -EINVAL;
	if (!resuid_valid(task, ruid, euid, suid))
		return -EPERM;

	if (ruid != -1)
		task->uid = (kuid_t)ruid;
	if (euid != -1)
		task->euid = (kuid_t)euid;
	if (suid != -1)
		task->suid = (kuid_t)suid;
	task->fsuid = task->euid;
	update_caps_after_setuid(task, old_euid);
	return 0;
}

static bool resgid_valid(struct task *task, int rgid, int egid, int sgid)
{
	if (capable(CAP_SETGID))
		return true;

	if (rgid != -1 && (kgid_t)rgid != task->gid &&
	    (kgid_t)rgid != task->egid && (kgid_t)rgid != task->sgid)
		return false;
	if (egid != -1 && (kgid_t)egid != task->gid &&
	    (kgid_t)egid != task->egid && (kgid_t)egid != task->sgid)
		return false;
	if (sgid != -1 && (kgid_t)sgid != task->gid &&
	    (kgid_t)sgid != task->egid && (kgid_t)sgid != task->sgid)
		return false;
	return true;
}

u64 sys_setresgid(void)
{
	struct task *task = current_task();
	int rgid = syscall_arg_int(0);
	int egid = syscall_arg_int(1);
	int sgid = syscall_arg_int(2);

	if (rgid < -1 || egid < -1 || sgid < -1)
		return -EINVAL;
	if (!resgid_valid(task, rgid, egid, sgid))
		return -EPERM;

	if (rgid != -1)
		task->gid = (kgid_t)rgid;
	if (egid != -1)
		task->egid = (kgid_t)egid;
	if (sgid != -1)
		task->sgid = (kgid_t)sgid;
	task->fsgid = task->egid;
	return 0;
}

u64 sys_getgroups(void)
{
	struct task *task = current_task();
	int size = syscall_arg_int(0);
	u32 *list = syscall_arg_ptr(1);

	if (size < 0)
		return -EINVAL;
	if (size == 0)
		return task->ngroups;
	if (size < task->ngroups)
		return -EINVAL;
	if (!list)
		return -EFAULT;

	for (int i = 0; i < task->ngroups; i++) {
		u32 gid = task->groups[i];

		if (copy_to_user(&list[i], &gid, sizeof(gid)))
			return -EFAULT;
	}
	return task->ngroups;
}

u64 sys_umask(void)
{
	struct task *task = current_task();
	umode_t old = task->umask;

	task->umask = syscall_arg_raw(0) & 0777;
	return old;
}

u64 sys_setpgid(void)
{
	pid_t pid = syscall_arg_int(0);
	pid_t pgid = syscall_arg_int(1);
	struct task *task = current_task();

	if (pid != 0 && pid != task->tgid)
		return -ESRCH;
	if (pgid < 0)
		return -EINVAL;
	if (pgid == 0)
		pgid = task->pid;

	/* A session leader cannot change its process group. */
	if (task->sid == task->pid)
		return -EPERM;
	if (pgid != task->pid && !task_pgrp_exists(pgid, task->sid))
		return -EPERM;

	task->pgid = pgid;
	return 0;
}

u64 sys_setsid(void)
{
	struct task *task = current_task();

	if (task->pgid == task->pid)
		return -EPERM;

	task->sid = task->pid;
	task->pgid = task->pid;
	return task->sid;
}

u64 sys_getpgid(void)
{
	pid_t pid = syscall_arg_int(0);

	if (pid == 0)
		return current_task()->pgid;
	return task_get_pgid(pid);
}

u64 sys_getsid(void)
{
	pid_t pid = syscall_arg_int(0);

	if (pid == 0)
		return current_task()->sid;
	return task_get_sid(pid);
}
