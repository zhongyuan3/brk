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
	[SYS_READ_IOV] = sys_readv,
	[SYS_WRITE_IOV] = sys_writev,
	[SYS_EXIT] = sys_exit,
	[SYS_EXIT_ALL] = sys_exit_group,
	[SYS_GET_TID] = sys_gettid,
	[SYS_CREATE_THREAD] = sys_thread_create,
	[SYS_FUTEX] = sys_futex,
	[SYS_CLOSE] = sys_close,
	[SYS_SET_SIGNAL] = sys_set_signal,
	[SYS_SET_SIGMASK] = sys_set_sigmask,
	[SYS_GET_PENDING] = sys_get_pending,
	[SYS_WAIT_SIGNAL] = sys_wait_signal,
	[SYS_RESTORE_CONTEXT] = sys_restore_context,
	[SYS_IOCTL] = sys_ioctl,
	[SYS_TRUNCATE] = sys_truncate,
	[SYS_TRUNCATE_FD] = sys_ftruncate,
	[SYS_CHMOD] = sys_fchmodat,
	[SYS_CHOWN] = sys_fchownat,
	[SYS_SET_FILE_TIMES] = sys_utimensat,
	[SYS_OPEN] = sys_openat,
	[SYS_DUP] = sys_dup,
	[SYS_DUP_TO] = sys_dup3,
	[SYS_EXEC] = sys_execve,
	[SYS_GET_PROC] = sys_getproc,
	[SYS_SET_PGID] = sys_setpgid,
	[SYS_SET_SID] = sys_setsid,
	[SYS_GET_PGID] = sys_getpgid,
	[SYS_GET_SID] = sys_getsid,
	[SYS_MKDIR] = sys_mkdirat,
	[SYS_CHDIR] = sys_chdir,
	[SYS_GET_CWD] = sys_getcwd,
	[SYS_MKNOD] = sys_mknodat,
	[SYS_LINK] = sys_linkat,
	[SYS_UNLINK] = sys_unlinkat,
	[SYS_PIPE] = sys_pipe2,
	[SYS_GET_SYSINFO] = sys_sysinfo,
	[SYS_READ_DIR] = sys_getdents,
	[SYS_WAIT] = sys_waitid,
	[SYS_FORK] = sys_fork,
	[SYS_GET_TIMES] = sys_get_times,
	[SYS_STAT] = sys_statx,
	[SYS_ACCESS] = sys_faccessat,
	[SYS_STAT_FS] = sys_statfs,
	[SYS_STAT_FS_FD] = sys_fstatfs,
	[SYS_PREALLOCATE] = sys_fallocate,
	[SYS_GET_CLOCK] = sys_clock_gettime,
	[SYS_SET_CLOCK] = sys_clock_settime,
	[SYS_SLEEP_CLOCK] = sys_clock_nanosleep,
	[SYS_MMAP] = sys_mmap,
	[SYS_MUNMAP] = sys_munmap,
	[SYS_MSYNC] = sys_msync,
	[SYS_YIELD] = sys_sched_yield,
	[SYS_SEND_SIGNAL] = sys_kill,
	[SYS_CHDIR_FD] = sys_fchdir,
	[SYS_MPROTECT] = sys_mprotect,
	[SYS_RENAME] = sys_renameat,
	[SYS_SYMLINK] = sys_symlinkat,
	[SYS_READLINK] = sys_readlinkat,
	[SYS_SEEK] = sys_lseek,
	[SYS_SYNC_FD] = sys_fsync,
	[SYS_SYNC_DATA_FD] = sys_fdatasync,
	[SYS_SYNC] = sys_sync,
	[SYS_SET_UID_REAL] = sys_set_uid_real,
	[SYS_SET_GID_REAL] = sys_set_gid_real,
	[SYS_SET_UID_EFFECTIVE] = sys_set_uid_effective,
	[SYS_SET_GID_EFFECTIVE] = sys_set_gid_effective,
	[SYS_SET_UID_SAVED] = sys_set_uid_saved,
	[SYS_SET_GID_SAVED] = sys_set_gid_saved,
	[SYS_GET_GROUPS] = sys_getgroups,
	[SYS_SET_UMASK] = sys_umask,
	[SYS_CREATE_SOCKET] = sys_socket,
	[SYS_CONNECT] = sys_connect,
	[SYS_ACCEPT] = sys_accept4,
	[SYS_SEND_TO] = sys_sendto,
	[SYS_RECV_FROM] = sys_recvfrom,
	[SYS_SHUTDOWN] = sys_shutdown,
	[SYS_BIND] = sys_bind,
	[SYS_LISTEN] = sys_listen,
	[SYS_GET_SOCK_ADDR] = sys_getsockname,
	[SYS_GET_PEER_ADDR] = sys_getpeername,
	[SYS_SET_SOCK_OPT] = sys_setsockopt,
	[SYS_GET_SOCK_OPT] = sys_getsockopt,
	[SYS_POLL] = sys_ppoll,
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
