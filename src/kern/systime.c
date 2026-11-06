#include <arch/pgtable.h>
#include <brk/ktime.h>
#include <brk/permission.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/resource.h>
#include <uapi/time.h>

u64 sys_gettimeofday(void)
{
	struct timeval *utv = syscall_arg_ptr(0);
	struct timeval tv;

	if (!utv)
		return 0;

	ktime_to_timeval(ktime_get_real(), &tv);
	if (copy_to_user(utv, &tv, sizeof(tv)))
		return -EFAULT;
	return 0;
}

u64 sys_settimeofday(void)
{
	const struct timeval *utv = syscall_arg_ptr(0);
	struct timeval tv;

	if (!capable(CAP_SYS_TIME))
		return -EPERM;

	if (!utv)
		return 0;
	if (copy_from_user(&tv, utv, sizeof(tv)))
		return -EFAULT;
	if (tv.tv_usec < 0 || tv.tv_usec >= (__kernel_suseconds_t)US_PER_SEC)
		return -EINVAL;

	ktime_set_real(ktime_from_timeval(&tv));
	return 0;
}

u64 sys_times(void)
{
	struct tms *buf;
	int err;

	buf = syscall_arg_ptr(0);
	err = task_get_times(current_task(), buf);
	if (err)
		return -1;
	return jiffies_get();
}

u64 sys_nanosleep(void)
{
	const struct timespec *udur = syscall_arg_ptr(0);
	struct timespec *urem = syscall_arg_ptr(1);
	struct timespec ts;
	ktime_t rem;
	int ret;

	if (!udur)
		return -EFAULT;
	if (copy_from_user(&ts, udur, sizeof(ts)))
		return -EFAULT;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 ||
	    ts.tv_nsec >= (__kernel_long_t)NS_PER_SEC)
		return -EINVAL;

	ret = ktime_nanosleep(ktime_from_timespec(&ts), &rem);
	if (ret == -EINTR && urem) {
		ktime_to_timespec(rem, &ts);
		if (copy_to_user(urem, &ts, sizeof(ts)))
			return -EFAULT;
	}

	return (u64)(long)ret;
}
