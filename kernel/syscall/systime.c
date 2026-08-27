#include <arch/pgtable.h>
#include <brk/base/kernel.h>
#include <brk/lib/string.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/mm_types.h>
#include <brk/mm/pgalloc.h>
#include <brk/mm/vmalloc.h>
#include <brk/printk/printk.h>
#include <brk/process/task.h>
#include <brk/syscall/syscall.h>
#include <brk/time/ktime.h>
#include <brk/time/timekeeper.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/resource.h>
#include <uapi/brk/time.h>

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
