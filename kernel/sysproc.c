#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/errno.h>
#include <aosd/mm_types.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
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
	return task_set_brk(addr);
}

uint64_t sys_sbrk(void)
{
	intptr_t incr = syscall_arg_raw(0);
	uint64_t curr_brk = current_task()->mm->brk;
	if (incr == 0)
		return curr_brk;

	uint64_t new_brk;
	if (incr < 0)
		new_brk = curr_brk - (uint64_t)(-incr);
	else
		new_brk = curr_brk + (uint64_t)incr;

	int err = task_set_brk(new_brk);
	if (err)
		return -1;

	return curr_brk;
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
	return do_wait4(pid, wstatus, opts, rus);
}

uint64_t sys_fork(void)
{
	return task_fork();
}

uint64_t sys_exit(void)
{
	int status = syscall_arg_raw(0);
	sched_exit(status);
	return 0;
}

uint64_t sys_nanosleep(void)
{
	struct timeval *dur, *rem;

	dur = (struct timeval *)syscall_arg_raw(0);
	rem = (struct timeval *)syscall_arg_raw(1);

	return do_nanosleep(dur, rem);
}

uint64_t sys_gettimeofday(void)
{
	struct timeval *tv;

	tv = (struct timeval *)syscall_arg_raw(0);
	walltime_get(tv);
	return 0;
}

uint64_t sys_settimeofday(void)
{
	const struct timeval *tv;

	tv = (const struct timeval *)syscall_arg_raw(0);
	walltime_set(tv);
	return 0;
}

uint64_t sys_times(void)
{
	struct tms *buf;

	buf = (struct tms *)syscall_arg_raw(0);
	*buf = current_task()->proc_tms;
	return 0;
}

uint64_t sys_kill(void)
{
	return -1;
}

uint64_t sys_shutdown(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_sched_yield(void)
{
	sched_yield();
	return 0;
}

uint64_t sys_getpid(void)
{
	return current_task()->pid;
}

uint64_t sys_getppid(void)
{
	return current_task()->parent->pid;
}
