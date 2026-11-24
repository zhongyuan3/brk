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
	[SYS_READ_IOV] = sys_read_iov,
	[SYS_WRITE_IOV] = sys_write_iov,
	[SYS_EXIT] = sys_exit,
	[SYS_EXIT_ALL] = sys_exit_all,
	[SYS_CREATE_THREAD] = sys_create_thread,
	[SYS_FUTEX] = sys_futex,
	[SYS_CLOSE] = sys_close,
	[SYS_SET_SIGNAL] = sys_set_signal,
	[SYS_SET_SIGMASK] = sys_set_sigmask,
	[SYS_GET_PENDING] = sys_get_pending,
	[SYS_WAIT_SIGNAL] = sys_wait_signal,
	[SYS_RESTORE_CONTEXT] = sys_restore_context,
	[SYS_IOCTL] = sys_ioctl,
	[SYS_TRUNCATE] = sys_truncate,
	[SYS_TRUNCATE_FD] = sys_truncate_fd,
	[SYS_CHMOD] = sys_chmod,
	[SYS_CHOWN] = sys_chown,
	[SYS_SET_FILE_TIMES] = sys_set_file_times,
	[SYS_OPEN] = sys_open,
	[SYS_DUP] = sys_dup,
	[SYS_DUP_TO] = sys_dup_to,
	[SYS_EXEC] = sys_exec,
	[SYS_GET_PROC] = sys_get_proc,
	[SYS_SET_PGID] = sys_set_pgid,
	[SYS_SET_SID] = sys_set_sid,
	[SYS_GET_PGID] = sys_get_pgid,
	[SYS_GET_SID] = sys_get_sid,
	[SYS_MKDIR] = sys_mkdir,
	[SYS_CHDIR] = sys_chdir,
	[SYS_GET_CWD] = sys_get_cwd,
	[SYS_MKNOD] = sys_mknod,
	[SYS_LINK] = sys_link,
	[SYS_UNLINK] = sys_unlink,
	[SYS_PIPE] = sys_pipe,
	[SYS_GET_SYSINFO] = sys_get_sysinfo,
	[SYS_READ_DIR] = sys_read_dir,
	[SYS_WAIT] = sys_wait,
	[SYS_FORK] = sys_fork,
	[SYS_GET_TIMES] = sys_get_times,
	[SYS_STAT] = sys_stat,
	[SYS_ACCESS] = sys_access,
	[SYS_STAT_FS] = sys_stat_fs,
	[SYS_STAT_FS_FD] = sys_stat_fs_fd,
	[SYS_PREALLOCATE] = sys_preallocate,
	[SYS_GET_CLOCK] = sys_get_clock,
	[SYS_SET_CLOCK] = sys_set_clock,
	[SYS_SLEEP_CLOCK] = sys_sleep_clock,
	[SYS_MMAP] = sys_mmap,
	[SYS_MUNMAP] = sys_munmap,
	[SYS_MSYNC] = sys_msync,
	[SYS_YIELD] = sys_yield,
	[SYS_SEND_SIGNAL] = sys_send_signal,
	[SYS_CHDIR_FD] = sys_chdir_fd,
	[SYS_MPROTECT] = sys_mprotect,
	[SYS_RENAME] = sys_rename,
	[SYS_SYMLINK] = sys_symlink,
	[SYS_READLINK] = sys_readlink,
	[SYS_SEEK] = sys_seek,
	[SYS_SYNC_FD] = sys_sync_fd,
	[SYS_SYNC_DATA_FD] = sys_sync_data_fd,
	[SYS_SYNC] = sys_sync,
	[SYS_SET_UID_REAL] = sys_set_uid_real,
	[SYS_SET_GID_REAL] = sys_set_gid_real,
	[SYS_SET_UID_EFFECTIVE] = sys_set_uid_effective,
	[SYS_SET_GID_EFFECTIVE] = sys_set_gid_effective,
	[SYS_SET_UID_SAVED] = sys_set_uid_saved,
	[SYS_SET_GID_SAVED] = sys_set_gid_saved,
	[SYS_SET_UMASK] = sys_set_umask,
	[SYS_CREATE_SOCKET] = sys_create_socket,
	[SYS_CONNECT] = sys_connect,
	[SYS_ACCEPT] = sys_accept,
	[SYS_SEND_TO] = sys_send_to,
	[SYS_RECV_FROM] = sys_recv_from,
	[SYS_SHUTDOWN] = sys_shutdown,
	[SYS_BIND] = sys_bind,
	[SYS_LISTEN] = sys_listen,
	[SYS_GET_SOCK_ADDR] = sys_get_sock_addr,
	[SYS_GET_PEER_ADDR] = sys_get_peer_addr,
	[SYS_SET_SOCK_OPT] = sys_set_sock_opt,
	[SYS_GET_SOCK_OPT] = sys_get_sock_opt,
	[SYS_POLL] = sys_poll,
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
