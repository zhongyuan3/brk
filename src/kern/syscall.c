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
	[SYS_READ] = sys_read,
	[SYS_WRITE] = sys_write,
	[SYS_READV] = sys_readv,
	[SYS_WRITEV] = sys_writev,
	[SYS_EXIT] = sys_exit,
	[SYS_EXIT_GROUP] = sys_exit_group,
	[SYS_GETTID] = sys_gettid,
	[SYS_THREAD_CREATE] = sys_thread_create,
	[SYS_FUTEX] = sys_futex,
	[SYS_CLOSE] = sys_close,
	[SYS_SIGACTION] = sys_sigaction,
	[SYS_SIGPROCMASK] = sys_sigprocmask,
	[SYS_SIGPENDING] = sys_sigpending,
	[SYS_SIGTIMEDWAIT] = sys_sigtimedwait,
	[SYS_SIGSUSPEND] = sys_sigsuspend,
	[SYS_SIGRETURN] = sys_sigreturn,
	[SYS_IOCTL] = sys_ioctl,
	[SYS_TRUNCATE] = sys_truncate,
	[SYS_FTRUNCATE] = sys_ftruncate,
	[SYS_FCHMODAT] = sys_fchmodat,
	[SYS_FCHOWNAT] = sys_fchownat,
	[SYS_UTIMENSAT] = sys_utimensat,
	[SYS_OPENAT] = sys_openat,
	[SYS_DUP] = sys_dup,
	[SYS_DUP3] = sys_dup3,
	[SYS_EXECVE] = sys_execve,
	[SYS_GETPROC] = sys_getproc,
	[SYS_SETPGID] = sys_setpgid,
	[SYS_SETSID] = sys_setsid,
	[SYS_GETPGID] = sys_getpgid,
	[SYS_GETSID] = sys_getsid,
	[SYS_MKDIRAT] = sys_mkdirat,
	[SYS_CHDIR] = sys_chdir,
	[SYS_GETCWD] = sys_getcwd,
	[SYS_MKNODAT] = sys_mknodat,
	[SYS_LINKAT] = sys_linkat,
	[SYS_UNLINKAT] = sys_unlinkat,
	[SYS_PIPE2] = sys_pipe2,
	[SYS_SYSINFO] = sys_sysinfo,
	[SYS_GETDENTS] = sys_getdents,
	[SYS_WAITID] = sys_waitid,
	[SYS_FORK] = sys_fork,
	[SYS_PAUSE] = sys_pause,
	[SYS_GET_TIMES] = sys_get_times,
	[SYS_STATX] = sys_statx,
	[SYS_FACCESSAT] = sys_faccessat,
	[SYS_STATFS] = sys_statfs,
	[SYS_FSTATFS] = sys_fstatfs,
	[SYS_FALLOCATE] = sys_fallocate,
	[SYS_CLOCK_GETTIME] = sys_clock_gettime,
	[SYS_CLOCK_SETTIME] = sys_clock_settime,
	[SYS_CLOCK_NANOSLEEP] = sys_clock_nanosleep,
	[SYS_MMAP] = sys_mmap,
	[SYS_MUNMAP] = sys_munmap,
	[SYS_MSYNC] = sys_msync,
	[SYS_SCHED_YIELD] = sys_sched_yield,
	[SYS_KILL] = sys_kill,
	[SYS_FCHDIR] = sys_fchdir,
	[SYS_MPROTECT] = sys_mprotect,
	[SYS_RENAMEAT] = sys_renameat,
	[SYS_SYMLINKAT] = sys_symlinkat,
	[SYS_READLINKAT] = sys_readlinkat,
	[SYS_LSEEK] = sys_lseek,
	[SYS_FSYNC] = sys_fsync,
	[SYS_FDATASYNC] = sys_fdatasync,
	[SYS_SYNC] = sys_sync,
	[SYS_SETUID] = sys_setuid,
	[SYS_SETGID] = sys_setgid,
	[SYS_SETREUID] = sys_setreuid,
	[SYS_SETREGID] = sys_setregid,
	[SYS_SETRESUID] = sys_setresuid,
	[SYS_SETRESGID] = sys_setresgid,
	[SYS_GETGROUPS] = sys_getgroups,
	[SYS_UMASK] = sys_umask,
	[SYS_SOCKET] = sys_socket,
	[SYS_CONNECT] = sys_connect,
	[SYS_ACCEPT4] = sys_accept4,
	[SYS_SENDTO] = sys_sendto,
	[SYS_RECVFROM] = sys_recvfrom,
	[SYS_SHUTDOWN] = sys_shutdown,
	[SYS_BIND] = sys_bind,
	[SYS_LISTEN] = sys_listen,
	[SYS_GETSOCKNAME] = sys_getsockname,
	[SYS_GETPEERNAME] = sys_getpeername,
	[SYS_SETSOCKOPT] = sys_setsockopt,
	[SYS_GETSOCKOPT] = sys_getsockopt,
	[SYS_PPOLL] = sys_ppoll,
};

void syscall(void)
{
	struct thread *thread = current_thread();
	u64 num = trap_frame_get_syscall_no(thread->frame);
	u64 ret;

	if (num < lengthof(systable) && systable[num])
		ret = systable[num]();
	else
		ret = -ENOSYS;
	trap_frame_set_ret_val(thread->frame, ret);
}

u64 syscall_arg_raw(int argno)
{
	struct trap_frame *tf = current_thread()->frame;
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
