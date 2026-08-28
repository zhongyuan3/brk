#ifndef BRK_SYSCALL_H
#define BRK_SYSCALL_H

#include <brk/base/types.h>
#include <uapi/brk/syscall.h>

struct fs_file;

void syscall(void);
uint64_t syscall_arg_raw(int argno);
void *syscall_arg_ptr(int argno);
int syscall_arg_int(int argno);

uint64_t sys_read(void);
uint64_t sys_write(void);
uint64_t sys_exit(void);
uint64_t sys_open(void);
uint64_t sys_close(void);
uint64_t sys_ioctl(void);
uint64_t sys_fstat(void);
uint64_t sys_lstat(void);
uint64_t sys_openat(void);
uint64_t sys_stat(void);
uint64_t sys_dup(void);
uint64_t sys_dup2(void);
uint64_t sys_execve(void);
uint64_t sys_getpid(void);
uint64_t sys_getppid(void);
uint64_t sys_mkdir(void);
uint64_t sys_mkdirat(void);
uint64_t sys_chdir(void);
uint64_t sys_getcwd(void);
uint64_t sys_mknod(void);
uint64_t sys_mknodat(void);
uint64_t sys_link(void);
uint64_t sys_linkat(void);
uint64_t sys_unlink(void);
uint64_t sys_unlinkat(void);
uint64_t sys_symlink(void);
uint64_t sys_readlink(void);
uint64_t sys_rmdir(void);
uint64_t sys_rename(void);
uint64_t sys_creat(void);
uint64_t sys_pipe(void);
uint64_t sys_pipe2(void);
uint64_t sys_uname(void);
uint64_t sys_brk(void);
uint64_t sys_getdents(void);
uint64_t sys_getdents64(void);
uint64_t sys_wait4(void);
uint64_t sys_fork(void);
uint64_t sys_nanosleep(void);
uint64_t sys_times(void);
uint64_t sys_mount(void);
uint64_t sys_umount2(void);
uint64_t sys_lseek(void);
uint64_t sys_clone(void);
uint64_t sys_mmap(void);
uint64_t sys_munmap(void);
uint64_t sys_mprotect(void);
uint64_t sys_msync(void);
uint64_t sys_mremap(void);
uint64_t sys_gettimeofday(void);
uint64_t sys_settimeofday(void);
uint64_t sys_sched_yield(void);
uint64_t sys_kill(void);
uint64_t sys_rt_sigaction(void);
uint64_t sys_rt_sigprocmask(void);
uint64_t sys_rt_sigreturn(void);
uint64_t sys_fchdir(void);
uint64_t sys_renameat(void);
uint64_t sys_renameat2(void);
uint64_t sys_symlinkat(void);
uint64_t sys_readlinkat(void);
uint64_t sys_fsync(void);
uint64_t sys_fdatasync(void);
uint64_t sys_sync(void);
uint64_t sys_syncfs(void);
uint64_t sys_gettid(void);

#endif
