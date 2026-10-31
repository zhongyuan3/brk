#include <arch/trapframe.h>
#include <brk/cpu.h>
#include <brk/kernel.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <uapi/errno.h>

static u64 (*systable[])(void) = {
	[SYS_read] = sys_read,
	[SYS_write] = sys_write,
	[SYS_exit] = sys_exit,
	[SYS_open] = sys_open,
	[SYS_close] = sys_close,
	[SYS_rt_sigaction] = sys_rt_sigaction,
	[SYS_rt_sigprocmask] = sys_rt_sigprocmask,
	[SYS_rt_sigreturn] = sys_rt_sigreturn,
	[SYS_ioctl] = sys_ioctl,
	[SYS_truncate] = sys_truncate,
	[SYS_ftruncate] = sys_ftruncate,
	[SYS_chmod] = sys_chmod,
	[SYS_fchmod] = sys_fchmod,
	[SYS_fchmodat] = sys_fchmodat,
	[SYS_chown] = sys_chown,
	[SYS_fchown] = sys_fchown,
	[SYS_fchownat] = sys_fchownat,
	[SYS_utime] = sys_utime,
	[SYS_utimes] = sys_utimes,
	[SYS_futimesat] = sys_futimesat,
	[SYS_utimensat] = sys_utimensat,
	[SYS_lstat] = sys_lstat,
	[SYS_openat] = sys_openat,
	[SYS_dup] = sys_dup,
	[SYS_dup2] = sys_dup2,
	[SYS_execve] = sys_execve,
	[SYS_getpid] = sys_getpid,
	[SYS_setpgid] = sys_setpgid,
	[SYS_getpgrp] = sys_getpgrp,
	[SYS_setsid] = sys_setsid,
	[SYS_getpgid] = sys_getpgid,
	[SYS_getsid] = sys_getsid,
	[SYS_getppid] = sys_getppid,
	[SYS_mkdir] = sys_mkdir,
	[SYS_mkdirat] = sys_mkdirat,
	[SYS_chdir] = sys_chdir,
	[SYS_getcwd] = sys_getcwd,
	[SYS_mknod] = sys_mknod,
	[SYS_mknodat] = sys_mknodat,
	[SYS_link] = sys_link,
	[SYS_linkat] = sys_linkat,
	[SYS_unlink] = sys_unlink,
	[SYS_unlinkat] = sys_unlinkat,
	[SYS_pipe] = sys_pipe,
	[SYS_pipe2] = sys_pipe2,
	[SYS_uname] = sys_uname,
	[SYS_brk] = sys_brk,
	[SYS_getdents] = sys_getdents,
	[SYS_getdents64] = sys_getdents64,
	[SYS_wait4] = sys_wait4,
	[SYS_fork] = sys_fork,
	[SYS_nanosleep] = sys_nanosleep,
	[SYS_times] = sys_times,
	[SYS_mount] = sys_mount,
	[SYS_umount2] = sys_umount2,
	[SYS_fstat] = sys_fstat,
	[SYS_stat] = sys_stat,
	[SYS_access] = sys_access,
	[SYS_faccessat] = sys_faccessat,
	[SYS_statfs] = sys_statfs,
	[SYS_fstatfs] = sys_fstatfs,
	[SYS_fallocate] = sys_fallocate,
	[SYS_gettimeofday] = sys_gettimeofday,
	[SYS_settimeofday] = sys_settimeofday,
	[SYS_clone] = sys_clone,
	[SYS_mmap] = sys_mmap,
	[SYS_munmap] = sys_munmap,
	[SYS_mremap] = sys_mremap,
	[SYS_msync] = sys_msync,
	[SYS_sched_yield] = sys_sched_yield,
	[SYS_kill] = sys_kill,
	[SYS_fchdir] = sys_fchdir,
	[SYS_rename] = sys_rename,
	[SYS_symlink] = sys_symlink,
	[SYS_readlink] = sys_readlink,
	[SYS_mprotect] = sys_mprotect,
	[SYS_renameat] = sys_renameat,
	[SYS_renameat2] = sys_renameat2,
	[SYS_symlinkat] = sys_symlinkat,
	[SYS_readlinkat] = sys_readlinkat,
	[SYS_lseek] = sys_lseek,
	[SYS_rmdir] = sys_rmdir,
	[SYS_creat] = sys_creat,
	[SYS_fsync] = sys_fsync,
	[SYS_fdatasync] = sys_fdatasync,
	[SYS_sync] = sys_sync,
	[SYS_syncfs] = sys_syncfs,
	[SYS_gettid] = sys_gettid,
	[SYS_getuid] = sys_getuid,
	[SYS_geteuid] = sys_geteuid,
	[SYS_getgid] = sys_getgid,
	[SYS_getegid] = sys_getegid,
	[SYS_setuid] = sys_setuid,
	[SYS_setgid] = sys_setgid,
	[SYS_setreuid] = sys_setreuid,
	[SYS_setregid] = sys_setregid,
	[SYS_setresuid] = sys_setresuid,
	[SYS_getresuid] = sys_getresuid,
	[SYS_setresgid] = sys_setresgid,
	[SYS_getresgid] = sys_getresgid,
	[SYS_setfsuid] = sys_setfsuid,
	[SYS_setfsgid] = sys_setfsgid,
	[SYS_getgroups] = sys_getgroups,
	[SYS_setgroups] = sys_setgroups,
	[SYS_umask] = sys_umask,
	[SYS_socket] = sys_socket,
	[SYS_connect] = sys_connect,
	[SYS_accept] = sys_accept,
	[SYS_accept4] = sys_accept4,
	[SYS_sendto] = sys_sendto,
	[SYS_recvfrom] = sys_recvfrom,
	[SYS_shutdown] = sys_shutdown,
	[SYS_bind] = sys_bind,
	[SYS_listen] = sys_listen,
	[SYS_getsockname] = sys_getsockname,
	[SYS_getpeername] = sys_getpeername,
	[SYS_setsockopt] = sys_setsockopt,
	[SYS_getsockopt] = sys_getsockopt,
	[SYS_poll] = sys_poll,
	[SYS_select] = sys_select,
};

void syscall(void)
{
	struct task *task = current_task();
	u64 num = task->frame->a7;
	u64 ret;

	if (num < countof(systable) && systable[num])
		ret = systable[num]();
	else
		ret = -ENOSYS;
	task->frame->a0 = ret;
}

u64 syscall_arg_raw(int argno)
{
	struct riscv_trap_frame *tf = current_task()->frame;
	switch (argno) {
	case 0:
		return tf->a0;
	case 1:
		return tf->a1;
	case 2:
		return tf->a2;
	case 3:
		return tf->a3;
	case 4:
		return tf->a4;
	case 5:
		return tf->a5;
	default:
		panic("%s(): illegal argument number\n", __func__);
	}
}

void *syscall_arg_ptr(int argno)
{
	return (void *)(uintptr_t)syscall_arg_raw(argno);
}

int syscall_arg_int(int argno)
{
	return (int)syscall_arg_raw(argno);
}
