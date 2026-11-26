#ifndef _BRK_SYSCALL_H
#define _BRK_SYSCALL_H

#include <brk/types.h>
#include <stddef.h>
#include <uapi/syscall.h>

struct file;
struct trap_frame;

#define SYSCALL_MAX_ARGS 6

struct syscall_ctx {
	struct trap_frame *regs;
	u64 nr;
	u64 args[SYSCALL_MAX_ARGS];
};

void syscall(void);

u64 syscall_arg_raw(const struct syscall_ctx *ctx, unsigned int argno);
void *syscall_arg_ptr(const struct syscall_ctx *ctx, unsigned int argno);
int syscall_arg_int(const struct syscall_ctx *ctx, unsigned int argno);
long syscall_arg_long(const struct syscall_ctx *ctx, unsigned int argno);
size_t syscall_arg_size(const struct syscall_ctx *ctx, unsigned int argno);
user_addr_t syscall_arg_user(const struct syscall_ctx *ctx, unsigned int argno);

long sys_read(const struct syscall_ctx *ctx);
long sys_write(const struct syscall_ctx *ctx);
long sys_exit(const struct syscall_ctx *ctx);
long sys_exit_all(const struct syscall_ctx *ctx);
long sys_create_thread(const struct syscall_ctx *ctx);
long sys_futex(const struct syscall_ctx *ctx);
long sys_close(const struct syscall_ctx *ctx);
long sys_ioctl(const struct syscall_ctx *ctx);
long sys_truncate(const struct syscall_ctx *ctx);
long sys_truncate_fd(const struct syscall_ctx *ctx);
long sys_chmod(const struct syscall_ctx *ctx);
long sys_set_file_times(const struct syscall_ctx *ctx);
long sys_stat(const struct syscall_ctx *ctx);
long sys_open(const struct syscall_ctx *ctx);
long sys_access(const struct syscall_ctx *ctx);
long sys_stat_fs(const struct syscall_ctx *ctx);
long sys_stat_fs_fd(const struct syscall_ctx *ctx);
long sys_preallocate(const struct syscall_ctx *ctx);
long sys_dup(const struct syscall_ctx *ctx);
long sys_exec(const struct syscall_ctx *ctx);
long sys_set_pgid(const struct syscall_ctx *ctx);
long sys_set_sid(const struct syscall_ctx *ctx);
long sys_get_pgid(const struct syscall_ctx *ctx);
long sys_get_sid(const struct syscall_ctx *ctx);
long sys_mkdir(const struct syscall_ctx *ctx);
long sys_chdir(const struct syscall_ctx *ctx);
long sys_get_cwd(const struct syscall_ctx *ctx);
long sys_mknod(const struct syscall_ctx *ctx);
long sys_link(const struct syscall_ctx *ctx);
long sys_unlink(const struct syscall_ctx *ctx);
long sys_pipe(const struct syscall_ctx *ctx);
long sys_get_sysinfo(const struct syscall_ctx *ctx);
long sys_read_dir(const struct syscall_ctx *ctx);
long sys_wait(const struct syscall_ctx *ctx);
long sys_fork(const struct syscall_ctx *ctx);
long sys_seek(const struct syscall_ctx *ctx);
long sys_mmap(const struct syscall_ctx *ctx);
long sys_munmap(const struct syscall_ctx *ctx);
long sys_mprotect(const struct syscall_ctx *ctx);
long sys_msync(const struct syscall_ctx *ctx);
long sys_yield(const struct syscall_ctx *ctx);
long sys_send_signal(const struct syscall_ctx *ctx);
long sys_set_signal(const struct syscall_ctx *ctx);
long sys_set_sigmask(const struct syscall_ctx *ctx);
long sys_restore_context(const struct syscall_ctx *ctx);
long sys_chdir_fd(const struct syscall_ctx *ctx);
long sys_rename(const struct syscall_ctx *ctx);
long sys_symlink(const struct syscall_ctx *ctx);
long sys_readlink(const struct syscall_ctx *ctx);
long sys_sync_fd(const struct syscall_ctx *ctx);
long sys_sync_data_fd(const struct syscall_ctx *ctx);
long sys_sync(const struct syscall_ctx *ctx);
long sys_set_uid_real(const struct syscall_ctx *ctx);
long sys_set_gid_real(const struct syscall_ctx *ctx);
long sys_set_uid_effective(const struct syscall_ctx *ctx);
long sys_set_gid_effective(const struct syscall_ctx *ctx);
long sys_set_uid_saved(const struct syscall_ctx *ctx);
long sys_set_gid_saved(const struct syscall_ctx *ctx);
long sys_get_proc(const struct syscall_ctx *ctx);
long sys_set_umask(const struct syscall_ctx *ctx);
long sys_chown(const struct syscall_ctx *ctx);

long sys_create_socket(const struct syscall_ctx *ctx);
long sys_bind(const struct syscall_ctx *ctx);
long sys_connect(const struct syscall_ctx *ctx);
long sys_listen(const struct syscall_ctx *ctx);
long sys_accept(const struct syscall_ctx *ctx);
long sys_send_to(const struct syscall_ctx *ctx);
long sys_recv_from(const struct syscall_ctx *ctx);
long sys_shutdown(const struct syscall_ctx *ctx);
long sys_get_sock_addr(const struct syscall_ctx *ctx);
long sys_get_peer_addr(const struct syscall_ctx *ctx);
long sys_set_sock_opt(const struct syscall_ctx *ctx);
long sys_get_sock_opt(const struct syscall_ctx *ctx);

long sys_poll(const struct syscall_ctx *ctx);

long sys_get_clock(const struct syscall_ctx *ctx);
long sys_set_clock(const struct syscall_ctx *ctx);
long sys_sleep_clock(const struct syscall_ctx *ctx);
long sys_get_times(const struct syscall_ctx *ctx);

long sys_read_iov(const struct syscall_ctx *ctx);
long sys_write_iov(const struct syscall_ctx *ctx);
long sys_dup_to(const struct syscall_ctx *ctx);

long sys_get_pending(const struct syscall_ctx *ctx);
long sys_wait_signal(const struct syscall_ctx *ctx);

#endif
