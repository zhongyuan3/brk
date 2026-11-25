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

static int clock_now(int clockid, ktime_t *now)
{
	struct process *process = current_process();

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
		*now = (ktime_t)(process_get_user_time(process) +
				 process_get_system_time(process)) *
		       (ktime_t)TICK_NS;
		return 0;
	default:
		return -EINVAL;
	}
}

long sys_get_clock(const struct syscall_ctx *ctx)
{
	int clockid = syscall_arg_int(ctx, 0);
	struct __brk_timespec *ut = syscall_arg_ptr(ctx, 1);
	struct __brk_timespec ts;
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

long sys_set_clock(const struct syscall_ctx *ctx)
{
	int clockid = syscall_arg_int(ctx, 0);
	const struct __brk_timespec *ut = syscall_arg_ptr(ctx, 1);
	struct __brk_timespec ts;

	if (clockid != CLOCK_REALTIME)
		return -EINVAL;
	if (!capable(CAP_SYS_TIME))
		return -EPERM;
	if (!ut)
		return -EFAULT;
	if (copy_from_user(&ts, ut, sizeof(ts)))
		return -EFAULT;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 ||
	    ts.tv_nsec >= (__brk_long_t)NS_PER_SEC)
		return -EINVAL;

	ktime_t t = ktime_from_timespec(&ts);
	return (u64)(long)ktime_set_realtime(t);
}

long sys_sleep_clock(const struct syscall_ctx *ctx)
{
	int clockid = syscall_arg_int(ctx, 0);
	int flags = syscall_arg_int(ctx, 1);
	const struct __brk_timespec *ureq = syscall_arg_ptr(ctx, 2);
	struct __brk_timespec *urem = syscall_arg_ptr(ctx, 3);
	struct __brk_timespec req;
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
	    req.tv_nsec >= (__brk_long_t)NS_PER_SEC)
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

long sys_get_times(const struct syscall_ctx *ctx)
{
	struct process *process = current_process();
	struct __brk_times *ubuf = syscall_arg_ptr(ctx, 0);
	struct __brk_times times = {
		.user_ns = process_get_user_time(process) * TICK_NS,
		.system_ns = process_get_system_time(process) * TICK_NS,
		.child_user_ns = process_get_child_user_time(process) * TICK_NS,
		.child_system_ns =
			process_get_child_system_time(process) * TICK_NS,
	};

	if (!ubuf)
		return -EFAULT;
	if (copy_to_user(ubuf, &times, sizeof(times)))
		return -EFAULT;
	return 0;
}
