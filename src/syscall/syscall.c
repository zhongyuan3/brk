#include <arch/syscall.h>
#include <brk/base/kernel.h>
#include <brk/printk/panic.h>
#include <brk/printk/printk.h>
#include <brk/process/task.h>
#include <brk/syscall/syscall.h>
#include <uapi/brk/errno.h>

static uint64_t (*systable[])(void) = {
	[SYS_read] = sys_read,
	[SYS_write] = sys_write,
	[SYS_exit] = sys_exit,
	[SYS_open] = sys_open,
	[SYS_close] = sys_close,
	[SYS_rt_sigaction] = sys_rt_sigaction,
	[SYS_rt_sigprocmask] = sys_rt_sigprocmask,
	[SYS_rt_sigreturn] = sys_rt_sigreturn,
	[SYS_ioctl] = sys_ioctl,
	[SYS_lstat] = sys_lstat,
	[SYS_openat] = sys_openat,
	[SYS_dup] = sys_dup,
	[SYS_dup2] = sys_dup2,
	[SYS_execve] = sys_execve,
	[SYS_getpid] = sys_getpid,
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
};

void syscall(void)
{
	struct task_control_block *task = current_task();
	uint64_t num = arch_syscall_get_nr(task->tf);
	uint64_t ret;

	if (num < countof(systable) && systable[num])
		ret = systable[num]();
	else
		ret = -ENOSYS;
	arch_syscall_set_ret(task->tf, ret);
}

uint64_t syscall_arg_raw(int argno)
{
	return arch_syscall_get_arg(current_task()->tf, argno);
}

void *syscall_arg_ptr(int argno)
{
	return (void *)(uintptr_t)syscall_arg_raw(argno);
}

int syscall_arg_int(int argno)
{
	return (int)syscall_arg_raw(argno);
}
