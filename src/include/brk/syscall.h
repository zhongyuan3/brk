#ifndef BRK_SYSCALL_H
#define BRK_SYSCALL_H

#include <brk/types.h>
#include <uapi/syscall.h>
#include <uapi/syscall_internal.h>

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
u64 sys_utimensat(void);
u64 sys_fstat(void);
u64 sys_lstat(void);
u64 sys_openat(void);
u64 sys_stat(void);
u64 sys_access(void);
u64 sys_faccessat(void);
u64 sys_statvfs(void);
u64 sys_fstatvfs(void);
u64 sys_posix_fallocate(void);
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
u64 sys_posix_getdents(void);
u64 sys_waitid(void);
u64 sys_fork(void);
u64 sys_nanosleep(void);
u64 sys_times(void);
u64 sys_lseek(void);
u64 sys_mmap(void);
u64 sys_munmap(void);
u64 sys_mprotect(void);
u64 sys_msync(void);
u64 sys_gettimeofday(void);
u64 sys_settimeofday(void);
u64 sys_sched_yield(void);
u64 sys_kill(void);
u64 sys_sigaction(void);
u64 sys_sigprocmask(void);
u64 sys_sigreturn(void);
u64 sys_fchdir(void);
u64 sys_renameat(void);
u64 sys_symlinkat(void);
u64 sys_readlinkat(void);
u64 sys_fsync(void);
u64 sys_fdatasync(void);
u64 sys_sync(void);
u64 sys_getuid(void);
u64 sys_geteuid(void);
u64 sys_getgid(void);
u64 sys_getegid(void);
u64 sys_setuid(void);
u64 sys_setgid(void);
u64 sys_setreuid(void);
u64 sys_setregid(void);
u64 sys_setresuid(void);
u64 sys_getresuid(void);
u64 sys_setresgid(void);
u64 sys_getresgid(void);
u64 sys_getgroups(void);
u64 sys_umask(void);
u64 sys_chown(void);
u64 sys_fchown(void);
u64 sys_fchownat(void);

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
u64 sys_pselect(void);
u64 sys_ppoll(void);

u64 sys_time(void);
u64 sys_clock_gettime(void);
u64 sys_clock_getres(void);
u64 sys_clock_settime(void);
u64 sys_clock_nanosleep(void);
u64 sys_getrusage(void);

u64 sys_futimens(void);
u64 sys_fstatat(void);
u64 sys_lchown(void);
u64 sys_mkfifo(void);
u64 sys_readv(void);
u64 sys_writev(void);
u64 sys_dup3(void);

u64 sys_mlock(void);
u64 sys_munlock(void);
u64 sys_mlockall(void);
u64 sys_munlockall(void);

u64 sys_sigpending(void);
u64 sys_pause(void);

#endif
