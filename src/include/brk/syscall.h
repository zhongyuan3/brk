#ifndef BRK_SYSCALL_H
#define BRK_SYSCALL_H

#include <brk/types.h>
#include <uapi/syscall.h>

struct fs_file;

void syscall(void);
u64 syscall_arg_raw(int argno);
void *syscall_arg_ptr(int argno);
int syscall_arg_int(int argno);

u64 sys_read(void);
u64 sys_write(void);
u64 sys_exit(void);
u64 sys_open(void);
u64 sys_close(void);
u64 sys_ioctl(void);
u64 sys_truncate(void);
u64 sys_ftruncate(void);
u64 sys_chmod(void);
u64 sys_fchmod(void);
u64 sys_fchmodat(void);
u64 sys_utime(void);
u64 sys_utimes(void);
u64 sys_futimesat(void);
u64 sys_utimensat(void);
u64 sys_fstat(void);
u64 sys_lstat(void);
u64 sys_openat(void);
u64 sys_stat(void);
u64 sys_statfs(void);
u64 sys_fstatfs(void);
u64 sys_fallocate(void);
u64 sys_dup(void);
u64 sys_dup2(void);
u64 sys_execve(void);
u64 sys_getpid(void);
u64 sys_setpgid(void);
u64 sys_getpgrp(void);
u64 sys_setsid(void);
u64 sys_getpgid(void);
u64 sys_getsid(void);
u64 sys_getppid(void);
u64 sys_mkdir(void);
u64 sys_mkdirat(void);
u64 sys_chdir(void);
u64 sys_getcwd(void);
u64 sys_mknod(void);
u64 sys_mknodat(void);
u64 sys_link(void);
u64 sys_linkat(void);
u64 sys_unlink(void);
u64 sys_unlinkat(void);
u64 sys_symlink(void);
u64 sys_readlink(void);
u64 sys_rmdir(void);
u64 sys_rename(void);
u64 sys_creat(void);
u64 sys_pipe(void);
u64 sys_pipe2(void);
u64 sys_uname(void);
u64 sys_brk(void);
u64 sys_getdents(void);
u64 sys_getdents64(void);
u64 sys_wait4(void);
u64 sys_fork(void);
u64 sys_nanosleep(void);
u64 sys_times(void);
u64 sys_mount(void);
u64 sys_umount2(void);
u64 sys_lseek(void);
u64 sys_clone(void);
u64 sys_mmap(void);
u64 sys_munmap(void);
u64 sys_mprotect(void);
u64 sys_msync(void);
u64 sys_mremap(void);
u64 sys_gettimeofday(void);
u64 sys_settimeofday(void);
u64 sys_sched_yield(void);
u64 sys_kill(void);
u64 sys_rt_sigaction(void);
u64 sys_rt_sigprocmask(void);
u64 sys_rt_sigreturn(void);
u64 sys_fchdir(void);
u64 sys_renameat(void);
u64 sys_renameat2(void);
u64 sys_symlinkat(void);
u64 sys_readlinkat(void);
u64 sys_fsync(void);
u64 sys_fdatasync(void);
u64 sys_sync(void);
u64 sys_syncfs(void);
u64 sys_gettid(void);

u64 sys_socket(void);
u64 sys_bind(void);
u64 sys_connect(void);
u64 sys_listen(void);
u64 sys_accept(void);
u64 sys_accept4(void);
u64 sys_sendto(void);
u64 sys_recvfrom(void);
u64 sys_shutdown(void);
u64 sys_getsockname(void);
u64 sys_getpeername(void);
u64 sys_setsockopt(void);
u64 sys_getsockopt(void);

u64 sys_poll(void);
u64 sys_select(void);

#endif
