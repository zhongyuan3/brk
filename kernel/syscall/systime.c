#include <brk/align.h>
#include <brk/asm.h>
#include <brk/errno.h>
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
	*buf = current_process()->ptms;
	return 0;
}
