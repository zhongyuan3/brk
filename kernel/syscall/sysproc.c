#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/errno.h>
#include <aosd/mm_types.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/process_types.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/syscall.h>
#include <aosd/timer.h>
#include <aosd/vmalloc.h>
#include <uapi/aosd/resource.h>
#include <uapi/aosd/time.h>

uint64_t sys_brk(void)
{
	uint64_t addr = syscall_arg_raw(0);
	if (addr == 0)
		return proc_get_current()->mm->brk;
	return proc_set_brk(addr);
}

uint64_t sys_clone(void)
{
	return -EOPNOTSUPP;
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
	return -1;
}

uint64_t sys_sched_yield(void)
{
	proc_yield();
	return 0;
}

uint64_t sys_getpid(void)
{
	return proc_get_current()->pid;
}

uint64_t sys_getppid(void)
{
	return proc_get_current()->parent->pid;
}
