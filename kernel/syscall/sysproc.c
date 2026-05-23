#include <brk/asm.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/signal.h>
#include <brk/timer.h>
#include <brk/vmalloc.h>
#include <uapi/brk/errno.h>
#include <uapi/resource.h>
#include <uapi/time.h>

u64 sys_brk(void)
{
	u64 addr = syscall_arg_raw(0);
	if (addr == 0)
		return current_process()->mm->brk;
	return proc_set_brk(addr);
}

u64 sys_clone(void)
{
	return -ENOSYS;
}

u64 sys_wait4(void)
{
	pid_t pid = syscall_arg_raw(0);
	int *wstatus = (int *)syscall_arg_raw(1);
	int opts = syscall_arg_raw(2);
	struct rusage *rus = (struct rusage *)syscall_arg_raw(3);
	return proc_wait(pid, wstatus, opts, rus);
}

u64 sys_fork(void)
{
	return proc_fork();
}

u64 sys_exit(void)
{
	int status = syscall_arg_raw(0);

	proc_exit_normal(status);
	return 0;
}

u64 sys_kill(void)
{
	pid_t pid = syscall_arg_int(0);
	int sig = syscall_arg_int(1);

	return proc_kill(pid, sig);
}

u64 sys_sched_yield(void)
{
	proc_yield();
	return 0;
}

u64 sys_getpid(void)
{
	return current_process()->pid;
}

u64 sys_getppid(void)
{
	return current_process()->parent->pid;
}
