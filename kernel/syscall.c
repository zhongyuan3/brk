#include <aosd/cpu.h>
#include <aosd/errno.h>
#include <aosd/macros.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
#include <aosd/syscall.h>
#include <uapi/aosd/syscall.h>

static uint64_t (*systable[])(void) = {
	[SYS_READ] = sys_read,
	[SYS_WRITE] = sys_write,
	[SYS_EXIT] = sys_exit,
	[SYS_OPEN] = sys_open,
	[SYS_CLOSE] = sys_close,
	[SYS_LSTAT] = sys_lstat,
	[SYS_OPENAT] = sys_openat,
	[SYS_DUP] = sys_dup,
	[SYS_DUP2] = sys_dup2,
	[SYS_EXECVE] = sys_execve,
	[SYS_GETPID] = sys_getpid,
	[SYS_GETPPID] = sys_getppid,
	[SYS_MKDIR] = sys_mkdir,
	[SYS_MKDIRAT] = sys_mkdirat,
	[SYS_CHDIR] = sys_chdir,
	[SYS_GETCWD] = sys_getcwd,
	[SYS_MKNOD] = sys_mknod,
	[SYS_MKNODAT] = sys_mknodat,
	[SYS_LINK] = sys_link,
	[SYS_LINKAT] = sys_linkat,
	[SYS_UNLINK] = sys_unlink,
	[SYS_UNLINKAT] = sys_unlinkat,
	[SYS_PIPE] = sys_pipe,
	[SYS_PIPE2] = sys_pipe2,
	[SYS_UNAME] = sys_uname,
	[SYS_BRK] = sys_brk,
	[SYS_SBRK] = sys_sbrk,
	[SYS_GETDENTS64] = sys_getdents64,
	[SYS_WAIT4] = sys_wait4,
	[SYS_FORK] = sys_fork,
	[SYS_NANOSLEEP] = sys_nanosleep,
	[SYS_TIMES] = sys_times,
	[SYS_MOUNT] = sys_mount,
	[SYS_UMOUNT2] = sys_umount2,
	[SYS_FSTAT] = sys_fstat,
	[SYS_STAT] = sys_stat,
	[SYS_GETTIMEOFDAY] = sys_gettimeofday,
	[SYS_SETTIMEOFDAY] = sys_settimeofday,
	[SYS_CLONE] = sys_clone,
	[SYS_MMAP] = sys_mmap,
	[SYS_MUNMAP] = sys_munmap,
	[SYS_MREMAP] = sys_mremap,
	[SYS_MSYNC] = sys_msync,
	[SYS_SCHED_YIELD] = sys_sched_yield,
	[SYS_SHUTDOWN] = sys_shutdown,
	[SYS_KILL] = sys_kill,
	[SYS_FCHDIR] = sys_fchdir,
	[SYS_RENAME] = sys_rename,
	[SYS_SYMLINK] = sys_link,
	[SYS_READLINK] = sys_readlink,
	[SYS_MPROTECT] = sys_mprotect,
	[SYS_RENAMEAT] = sys_renameat,
	[SYS_SYMLINKAT] = sys_symlinkat,
	[SYS_READLINKAT] = sys_readlinkat,
	[SYS_LSEEK] = sys_lseek,
	[SYS_RMDIR] = sys_rmdir,
	[SYS_CREAT] = sys_creat,
};

void syscall(void)
{
	struct task *t = current_task();
	uint64_t n = t->tf.a7;
	if (n < countof(systable) && systable[n])
		t->tf.a0 = systable[n]();
	else
		t->tf.a0 = -ENOSYS;
}

uint64_t syscall_arg_raw(int argno)
{
	struct task *t = current_task();

	switch (argno) {
	case 0:
		return t->tf.a0;
	case 1:
		return t->tf.a1;
	case 2:
		return t->tf.a2;
	case 3:
		return t->tf.a3;
	case 4:
		return t->tf.a4;
	case 5:
		return t->tf.a5;
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

int syscall_arg_fd(int argno, int *pfd, struct file **pfp)
{
	int fd = syscall_arg_raw(argno);
	struct task *t = current_task();
	if (fd < 0 || fd > OPEN_MAX || !t->ofiles[fd])
		return -EBADF;
	if (pfd)
		*pfd = fd;
	if (pfp)
		*pfp = t->ofiles[fd];
	return 0;
}
