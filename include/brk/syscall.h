#ifndef BRK_SYSCALL_H
#define BRK_SYSCALL_H

#include <brk/types.h>
#include <uapi/syscall.h>

struct file;

void syscall(void);
u64 syscall_arg_raw(int argno);
void *syscall_arg_ptr(int argno);
int syscall_arg_int(int argno);

u64 sys_read(void);
u64 sys_write(void);
u64 sys_exit(void);
u64 sys_exit_all(void);
u64 sys_create_thread(void);
u64 sys_futex(void);
u64 sys_close(void);
u64 sys_ioctl(void);
u64 sys_truncate(void);
u64 sys_truncate_fd(void);
u64 sys_chmod(void);
u64 sys_set_file_times(void);
u64 sys_stat(void);
u64 sys_open(void);
u64 sys_access(void);
u64 sys_stat_fs(void);
u64 sys_stat_fs_fd(void);
u64 sys_preallocate(void);
u64 sys_dup(void);
u64 sys_exec(void);
u64 sys_set_pgid(void);
u64 sys_set_sid(void);
u64 sys_get_pgid(void);
u64 sys_get_sid(void);
u64 sys_mkdir(void);
u64 sys_chdir(void);
u64 sys_get_cwd(void);
u64 sys_mknod(void);
u64 sys_link(void);
u64 sys_unlink(void);
u64 sys_pipe(void);
u64 sys_get_sysinfo(void);
u64 sys_read_dir(void);
u64 sys_wait(void);
u64 sys_fork(void);
u64 sys_seek(void);
u64 sys_mmap(void);
u64 sys_munmap(void);
u64 sys_mprotect(void);
u64 sys_msync(void);
u64 sys_yield(void);
u64 sys_send_signal(void);
u64 sys_set_signal(void);
u64 sys_set_sigmask(void);
u64 sys_restore_context(void);
u64 sys_chdir_fd(void);
u64 sys_rename(void);
u64 sys_symlink(void);
u64 sys_readlink(void);
u64 sys_sync_fd(void);
u64 sys_sync_data_fd(void);
u64 sys_sync(void);
u64 sys_set_uid_real(void);
u64 sys_set_gid_real(void);
u64 sys_set_uid_effective(void);
u64 sys_set_gid_effective(void);
u64 sys_set_uid_saved(void);
u64 sys_set_gid_saved(void);
u64 sys_get_proc(void);
u64 sys_set_umask(void);
u64 sys_chown(void);

u64 sys_create_socket(void);
u64 sys_bind(void);
u64 sys_connect(void);
u64 sys_listen(void);
u64 sys_accept(void);
u64 sys_send_to(void);
u64 sys_recv_from(void);
u64 sys_shutdown(void);
u64 sys_get_sock_addr(void);
u64 sys_get_peer_addr(void);
u64 sys_set_sock_opt(void);
u64 sys_get_sock_opt(void);

u64 sys_poll(void);

u64 sys_get_clock(void);
u64 sys_set_clock(void);
u64 sys_sleep_clock(void);
u64 sys_get_times(void);

u64 sys_read_iov(void);
u64 sys_write_iov(void);
u64 sys_dup_to(void);

u64 sys_get_pending(void);
u64 sys_wait_signal(void);

#endif
