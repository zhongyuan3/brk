#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/errno.h>
#include <aosd/mm_types.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/resource.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/syscall.h>
#include <aosd/time.h>
#include <aosd/timer.h>
#include <aosd/vmalloc.h>

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
