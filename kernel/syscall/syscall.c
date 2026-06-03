#include <brk/cpu.h>
#include <brk/kernel.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <uapi/brk/errno.h>

static u64 (*systable[])(void) = {
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
	u64 num = task->tf->a7;
	if (num < countof(systable) && systable[num])
		task->tf->a0 = systable[num]();
	else
		task->tf->a0 = -ENOSYS;
}

u64 syscall_arg_raw(int argno)
{
	struct task_control_block *task = current_task();

	switch (argno) {
	case 0:
		return task->tf->a0;
	case 1:
		return task->tf->a1;
	case 2:
		return task->tf->a2;
	case 3:
		return task->tf->a3;
	case 4:
		return task->tf->a4;
	case 5:
		return task->tf->a5;
	default:
		panic("%s(): illegal argument number\n", __func__);
	}
}

void *syscall_arg_ptr(int argno)
{
	return (void *)syscall_arg_raw(argno);
}

int syscall_arg_int(int argno)
{
	return (int)syscall_arg_raw(argno);
}
