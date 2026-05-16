#include <brk/asm.h>
#include <brk/errno.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
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
#include <brk/vmalloc.h>

u64 sys_gettimeofday(void)
{
	struct timeval *tv;

	tv = syscall_arg_ptr(0);
	ktime_get_real_tv(tv);
	return 0;
}

u64 sys_settimeofday(void)
{
	const struct timeval *tv;

	tv = syscall_arg_ptr(0);
	ktime_set_real_tv(tv);
	return 0;
}

u64 sys_times(void)
{
	struct tms *buf;

	buf = syscall_arg_ptr(0);
	*buf = current_process()->ptms;
	return 0;
}

u64 sys_nanosleep(void)
{
	struct timespec *dur, *rem;

	dur = syscall_arg_ptr(0);
	rem = syscall_arg_ptr(1);
	if (!dur || !rem)
		return -EINVAL;

	return ktime_nanosleep(dur, rem);
}
