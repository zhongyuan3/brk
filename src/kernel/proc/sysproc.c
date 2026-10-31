#include <arch/pgtable.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/signal.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/uaccess.h>
#include <brk/vmalloc.h>
#include <uapi/errno.h>
#include <uapi/resource.h>
#include <uapi/time.h>

u64 sys_brk(void)
{
	u64 addr = syscall_arg_raw(0);
	if (addr == 0)
		return current_task()->mm->brk;
	return task_set_brk(addr);
}

u64 sys_clone(void)
{
	return -ENOSYS;
}

u64 sys_wait4(void)
{
	pid_t pid = syscall_arg_raw(0);
	int *wstatus = (int *)(uintptr_t)syscall_arg_raw(1);
	int opts = syscall_arg_raw(2);
	struct rusage *rus = (struct rusage *)(uintptr_t)syscall_arg_raw(3);
	return task_wait(pid, wstatus, opts, rus);
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

u64 sys_getpid(void)
{
	return current_task()->tgid;
}

u64 sys_getppid(void)
{
	struct task *par = current_task()->parent;

	return par ? par->tgid : 0;
}

u64 sys_gettid(void)
{
	return current_task()->pid;
}

u64 sys_getuid(void)
{
	return current_task()->uid;
}

u64 sys_geteuid(void)
{
	return current_task()->euid;
}

u64 sys_getgid(void)
{
	return current_task()->gid;
}

u64 sys_getegid(void)
{
	return current_task()->egid;
}

u64 sys_setuid(void)
{
	struct task *task = current_task();
	int uid = syscall_arg_int(0);

	if (uid < 0)
		return -EINVAL;

	if (task->euid == 0) {
		task->uid = task->euid = task->suid = (kuid_t)uid;
		task->fsuid = (kuid_t)uid;
	} else if ((kuid_t)uid == task->uid || (kuid_t)uid == task->suid) {
		task->euid = (kuid_t)uid;
		task->fsuid = (kuid_t)uid;
	} else {
		return -EPERM;
	}
	return 0;
}

u64 sys_setgid(void)
{
	struct task *task = current_task();
	int gid = syscall_arg_int(0);

	if (gid < 0)
		return -EINVAL;

	if (task->euid == 0) {
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

	if (ruid < -1 || euid < -1)
		return -EINVAL;
	if (task->euid != 0) {
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
	return 0;
}

u64 sys_setregid(void)
{
	struct task *task = current_task();
	int rgid = syscall_arg_int(0);
	int egid = syscall_arg_int(1);

	if (rgid < -1 || egid < -1)
		return -EINVAL;
	if (task->euid != 0) {
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
	if (task->euid == 0)
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
	return 0;
}

u64 sys_getresuid(void)
{
	struct task *task = current_task();
	u32 *ruid = syscall_arg_ptr(0);
	u32 *euid = syscall_arg_ptr(1);
	u32 *suid = syscall_arg_ptr(2);
	u32 id;

	id = task->uid;
	if (ruid && copy_to_user(ruid, &id, sizeof(id)))
		return -EFAULT;
	id = task->euid;
	if (euid && copy_to_user(euid, &id, sizeof(id)))
		return -EFAULT;
	id = task->suid;
	if (suid && copy_to_user(suid, &id, sizeof(id)))
		return -EFAULT;
	return 0;
}

static bool resgid_valid(struct task *task, int rgid, int egid, int sgid)
{
	if (task->euid == 0)
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

u64 sys_getresgid(void)
{
	struct task *task = current_task();
	u32 *rgid = syscall_arg_ptr(0);
	u32 *egid = syscall_arg_ptr(1);
	u32 *sgid = syscall_arg_ptr(2);
	u32 id;

	id = task->gid;
	if (rgid && copy_to_user(rgid, &id, sizeof(id)))
		return -EFAULT;
	id = task->egid;
	if (egid && copy_to_user(egid, &id, sizeof(id)))
		return -EFAULT;
	id = task->sgid;
	if (sgid && copy_to_user(sgid, &id, sizeof(id)))
		return -EFAULT;
	return 0;
}

u64 sys_setfsuid(void)
{
	struct task *task = current_task();
	int uid = syscall_arg_int(0);
	kuid_t old = task->fsuid;

	if (uid >= 0 &&
	    (task->euid == 0 || (kuid_t)uid == task->uid ||
	     (kuid_t)uid == task->euid || (kuid_t)uid == task->suid))
		task->fsuid = (kuid_t)uid;
	return old;
}

u64 sys_setfsgid(void)
{
	struct task *task = current_task();
	int gid = syscall_arg_int(0);
	kgid_t old = task->fsgid;

	if (gid >= 0 &&
	    (task->euid == 0 || (kgid_t)gid == task->gid ||
	     (kgid_t)gid == task->egid || (kgid_t)gid == task->sgid))
		task->fsgid = (kgid_t)gid;
	return old;
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

u64 sys_setgroups(void)
{
	struct task *task = current_task();
	int size = syscall_arg_int(0);
	const u32 *list = syscall_arg_ptr(1);
	kgid_t groups[NGROUPS_MAX];

	if (size < 0 || size > NGROUPS_MAX)
		return -EINVAL;
	if (task->euid != 0)
		return -EPERM;
	if (size && !list)
		return -EFAULT;

	if (size && copy_from_user(groups, list,
				   sizeof(groups[0]) * (size_t)size))
		return -EFAULT;

	for (int i = 0; i < size; i++)
		task->groups[i] = (kgid_t)groups[i];
	task->ngroups = size;
	return 0;
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

u64 sys_getpgrp(void)
{
	return current_task()->pgid;
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
