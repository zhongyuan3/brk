#include <arch/trapframe.h>
#include <brk/cpu.h>
#include <brk/lengthof.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <stdint.h>
#include <uapi/errno.h>

static u64 (*systable[])(void) = {
	[SYS_read] = sys_read,
	[SYS_write] = sys_write,
	[SYS_readv] = sys_readv,
	[SYS_writev] = sys_writev,
	[SYS_exit] = sys_exit,
	[SYS_close] = sys_close,
	[SYS_sigaction] = sys_sigaction,
	[SYS_sigprocmask] = sys_sigprocmask,
	[SYS_sigpending] = sys_sigpending,
	[SYS_sigreturn] = sys_sigreturn,
	[SYS_ioctl] = sys_ioctl,
	[SYS_truncate] = sys_truncate,
	[SYS_ftruncate] = sys_ftruncate,
	[SYS_fchmodat] = sys_fchmodat,
	[SYS_fchownat] = sys_fchownat,
	[SYS_utimensat] = sys_utimensat,
	[SYS_openat] = sys_openat,
	[SYS_dup] = sys_dup,
	[SYS_dup3] = sys_dup3,
	[SYS_execve] = sys_execve,
	[SYS_getpid] = sys_getpid,
	[SYS_setpgid] = sys_setpgid,
	[SYS_setsid] = sys_setsid,
	[SYS_getpgid] = sys_getpgid,
	[SYS_getsid] = sys_getsid,
	[SYS_getppid] = sys_getppid,
	[SYS_mkdirat] = sys_mkdirat,
	[SYS_chdir] = sys_chdir,
	[SYS_getcwd] = sys_getcwd,
	[SYS_mknodat] = sys_mknodat,
	[SYS_linkat] = sys_linkat,
	[SYS_unlinkat] = sys_unlinkat,
	[SYS_pipe2] = sys_pipe2,
	[SYS_uname] = sys_uname,
	[SYS_getdents] = sys_getdents,
	[SYS_waitid] = sys_waitid,
	[SYS_fork] = sys_fork,
	[SYS_pause] = sys_pause,
	[SYS_get_times] = sys_get_times,
	[SYS_statx] = sys_statx,
	[SYS_faccessat] = sys_faccessat,
	[SYS_statfs] = sys_statfs,
	[SYS_fstatfs] = sys_fstatfs,
	[SYS_posix_fallocate] = sys_posix_fallocate,
	[SYS_clock_gettime] = sys_clock_gettime,
	[SYS_clock_getres] = sys_clock_getres,
	[SYS_clock_settime] = sys_clock_settime,
	[SYS_clock_nanosleep] = sys_clock_nanosleep,
	[SYS_mmap] = sys_mmap,
	[SYS_munmap] = sys_munmap,
	[SYS_msync] = sys_msync,
	[SYS_sched_yield] = sys_sched_yield,
	[SYS_kill] = sys_kill,
	[SYS_fchdir] = sys_fchdir,
	[SYS_mprotect] = sys_mprotect,
	[SYS_renameat] = sys_renameat,
	[SYS_symlinkat] = sys_symlinkat,
	[SYS_readlinkat] = sys_readlinkat,
	[SYS_lseek] = sys_lseek,
	[SYS_fsync] = sys_fsync,
	[SYS_fdatasync] = sys_fdatasync,
	[SYS_sync] = sys_sync,
	[SYS_setuid] = sys_setuid,
	[SYS_setgid] = sys_setgid,
	[SYS_setreuid] = sys_setreuid,
	[SYS_setregid] = sys_setregid,
	[SYS_setresuid] = sys_setresuid,
	[SYS_getresuid] = sys_getresuid,
	[SYS_setresgid] = sys_setresgid,
	[SYS_getresgid] = sys_getresgid,
	[SYS_getgroups] = sys_getgroups,
	[SYS_umask] = sys_umask,
	[SYS_socket] = sys_socket,
	[SYS_connect] = sys_connect,
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
	[SYS_ppoll] = sys_ppoll,
};

void syscall(void)
{
	struct task *task = current_task();
	u64 num = trap_frame_get_syscall_no(task->frame);
	u64 ret;

	if (num < lengthof(systable) && systable[num])
		ret = systable[num]();
	else
		ret = -ENOSYS;
	trap_frame_set_ret_val(task->frame, ret);
}

u64 syscall_arg_raw(int argno)
{
	struct trap_frame *tf = current_task()->frame;
	return trap_frame_get_arg(tf, argno);
}

void *syscall_arg_ptr(int argno)
{
	return (void *)(uintptr_t)syscall_arg_raw(argno);
}

int syscall_arg_int(int argno)
{
	return (int)syscall_arg_raw(argno);
}
