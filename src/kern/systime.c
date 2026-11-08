#include <arch/pgtable.h>
#include <brk/clocksource.h>
#include <brk/ktime.h>
#include <brk/permission.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/tick.h>
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

	ktime_to_timeval(ktime_realtime(), &tv);
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
	if (tv.tv_sec < 0 || tv.tv_usec < 0 ||
	    tv.tv_usec >= (__kernel_suseconds_t)US_PER_SEC)
		return -EINVAL;

	ktime_t t = ktime_from_timeval(&tv);
	return (u64)(long)ktime_set_realtime(t);
}

u64 sys_times(void)
{
	struct tms *buf;
	int err;

	buf = syscall_arg_ptr(0);
	err = task_get_times(current_task(), buf);
	if (err)
		return -1;
	/* USER_HZ is defined as HZ, so jiffies are already user clock ticks. */
	return tick_get_jiffies();
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

u64 sys_time(void)
{
	time_t *ut = syscall_arg_ptr(0);
	time_t now = ktime_realtime_sec();

	if (ut && copy_to_user(ut, &now, sizeof(now)))
		return -EFAULT;
	return (u64)now;
}

static int clock_now(int clockid, ktime_t *now)
{
	struct task *task = current_task();

	switch (clockid) {
	case CLOCK_REALTIME:
		*now = ktime_realtime();
		return 0;
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		*now = (ktime_t)ktime_boottime_ns();
		return 0;
	case CLOCK_PROCESS_CPUTIME_ID:
	case CLOCK_THREAD_CPUTIME_ID:
		*now = (ktime_t)(task_get_user_time(task) +
				 task_get_system_time(task)) *
		       (ktime_t)TICK_NS;
		return 0;
	default:
		return -EINVAL;
	}
}

u64 sys_clock_gettime(void)
{
	int clockid = syscall_arg_int(0);
	struct timespec *ut = syscall_arg_ptr(1);
	struct timespec ts;
	ktime_t now;
	int ret;

	if (!ut)
		return -EFAULT;

	ret = clock_now(clockid, &now);
	if (ret)
		return ret;

	ktime_to_timespec(now, &ts);
	if (copy_to_user(ut, &ts, sizeof(ts)))
		return -EFAULT;
	return 0;
}

u64 sys_clock_getres(void)
{
	int clockid = syscall_arg_int(0);
	struct timespec *ut = syscall_arg_ptr(1);
	struct timespec ts;
	u64 res_ns;

	switch (clockid) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		res_ns = clocksource_get_resolution_ns();
		break;
	case CLOCK_PROCESS_CPUTIME_ID:
	case CLOCK_THREAD_CPUTIME_ID:
		res_ns = TICK_NS;
		break;
	default:
		return -EINVAL;
	}

	ktime_to_timespec((ktime_t)res_ns, &ts);
	if (ut && copy_to_user(ut, &ts, sizeof(ts)))
		return -EFAULT;
	return 0;
}

u64 sys_clock_settime(void)
{
	int clockid = syscall_arg_int(0);
	const struct timespec *ut = syscall_arg_ptr(1);
	struct timespec ts;

	if (clockid != CLOCK_REALTIME)
		return -EINVAL;
	if (!capable(CAP_SYS_TIME))
		return -EPERM;
	if (!ut)
		return -EFAULT;
	if (copy_from_user(&ts, ut, sizeof(ts)))
		return -EFAULT;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 ||
	    ts.tv_nsec >= (__kernel_long_t)NS_PER_SEC)
		return -EINVAL;

	ktime_t t = ktime_from_timespec(&ts);
	return (u64)(long)ktime_set_realtime(t);
}

u64 sys_clock_nanosleep(void)
{
	int clockid = syscall_arg_int(0);
	int flags = syscall_arg_int(1);
	const struct timespec *ureq = syscall_arg_ptr(2);
	struct timespec *urem = syscall_arg_ptr(3);
	struct timespec req;
	ktime_t now, target, duration, rem = 0;
	int ret;

	if (flags & ~TIMER_ABSTIME)
		return -EINVAL;
	if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC &&
	    clockid != CLOCK_BOOTTIME)
		return -EINVAL;
	if (!ureq)
		return -EFAULT;
	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;
	if (req.tv_sec < 0 || req.tv_nsec < 0 ||
	    req.tv_nsec >= (__kernel_long_t)NS_PER_SEC)
		return -EINVAL;

	target = ktime_from_timespec(&req);
	if (flags & TIMER_ABSTIME) {
		if (clockid == CLOCK_REALTIME)
			now = ktime_realtime();
		else
			now = (ktime_t)ktime_boottime_ns();
		duration = target - now;
		if (duration <= 0)
			return 0;
	} else {
		duration = target;
	}

	ret = ktime_nanosleep(duration, &rem);
	if (ret == -EINTR && !(flags & TIMER_ABSTIME) && urem) {
		ktime_to_timespec(rem, &req);
		if (copy_to_user(urem, &req, sizeof(req)))
			return -EFAULT;
	}

	return (u64)(long)ret;
}

static void jiffies_to_timeval(u64 ticks, struct timeval *tv)
{
	tv->tv_sec = (__kernel_time_t)(ticks / USER_HZ);
	tv->tv_usec = (__kernel_suseconds_t)((ticks % USER_HZ) *
					     (US_PER_SEC / USER_HZ));
}

u64 sys_getrusage(void)
{
	int who = syscall_arg_int(0);
	struct rusage *uru = syscall_arg_ptr(1);
	struct rusage ru = { 0 };
	struct task *task = current_task();
	u64 utime, stime;

	switch (who) {
	case RUSAGE_SELF:
		utime = task_get_user_time(task);
		stime = task_get_system_time(task);
		break;
	case RUSAGE_CHILDREN:
		utime = task_get_child_user_time(task);
		stime = task_get_child_system_time(task);
		break;
	default:
		return -EINVAL;
	}

	jiffies_to_timeval(utime, &ru.ru_utime);
	jiffies_to_timeval(stime, &ru.ru_stime);

	if (!uru)
		return -EFAULT;
	if (copy_to_user(uru, &ru, sizeof(ru)))
		return -EFAULT;
	return 0;
}
