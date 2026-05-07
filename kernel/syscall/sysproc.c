#include <brk/asm.h>
#include <brk/errno.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/resource.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/time.h>
#include <brk/timer.h>
#include <brk/vmalloc.h>

uint64_t sys_brk(void)
{
	uint64_t addr = syscall_arg_raw(0);
	if (addr == 0)
		return current_process()->mm->brk;
	return proc_set_brk(addr);
}

uint64_t sys_clone(void)
{
	return -ENOSYS;
}

uint64_t sys_wait4(void)
{
	pid_t pid = syscall_arg_raw(0);
	int *wstatus = (int *)syscall_arg_raw(1);
	int opts = syscall_arg_raw(2);
	struct rusage *rus = (struct rusage *)syscall_arg_raw(3);
	return proc_wait(pid, wstatus, opts, rus);
}

uint64_t sys_fork(void)
{
	return proc_fork();
}

uint64_t sys_exit(void)
{
	int status = syscall_arg_raw(0);
	proc_exit(status);
	return 0;
}

uint64_t sys_nanosleep(void)
{
	struct timeval *dur, *rem;

	dur = (struct timeval *)syscall_arg_raw(0);
	rem = (struct timeval *)syscall_arg_raw(1);

	return do_nanosleep(dur, rem);
}

uint64_t sys_kill(void)
{
	return -ENOSYS;
}

uint64_t sys_sched_yield(void)
{
	proc_yield();
	return 0;
}

uint64_t sys_getpid(void)
{
	return current_process()->pid;
}

uint64_t sys_getppid(void)
{
	return current_process()->parent->pid;
}
