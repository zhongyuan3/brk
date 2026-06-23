#include <arch/pgtable.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/vmalloc.h>
#include <uapi/brk/errno.h>
#include <uapi/resource.h>
#include <uapi/time.h>

uint64_t sys_gettimeofday(void)
{
	struct timeval *tv;

	tv = syscall_arg_ptr(0);
	ktime_get_real_tv(tv);
	return 0;
}

uint64_t sys_settimeofday(void)
{
	const struct timeval *tv;

	tv = syscall_arg_ptr(0);
	ktime_set_real_tv(tv);
	return 0;
}

uint64_t sys_times(void)
{
	struct tms *buf;
	int err;

	buf = syscall_arg_ptr(0);
	err = task_get_times(current_task(), buf);
	if (err)
		return -1;
	return jiffies_get();
}

uint64_t sys_nanosleep(void)
{
	struct timespec *dur, *rem;

	dur = syscall_arg_ptr(0);
	rem = syscall_arg_ptr(1);
	if (!dur || !rem)
		return -EINVAL;

	return ktime_nanosleep(dur, rem);
}
