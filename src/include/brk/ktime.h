#ifndef BRK_KTIME_H
#define BRK_KTIME_H

#include <brk/stat_types.h>
#include <brk/timeconst.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <uapi/time.h>

/*
 * ktime_t is signed nanoseconds internally.  struct timespec/timeval only
 * appear at the syscall boundary; these helpers are the explicit crossing
 * points.
 */
static inline ktime_t ktime_from_timespec(const struct timespec *ts)
{
	return (ktime_t)ts->tv_sec * (ktime_t)NS_PER_SEC + (ktime_t)ts->tv_nsec;
}

static inline void ktime_to_timespec(ktime_t t, struct timespec *ts)
{
	ts->tv_sec = (__kernel_time_t)(t / (ktime_t)NS_PER_SEC);
	ts->tv_nsec = (__kernel_long_t)(t % (ktime_t)NS_PER_SEC);
}

static inline ktime_t ktime_from_timeval(const struct timeval *tv)
{
	return (ktime_t)tv->tv_sec * (ktime_t)NS_PER_SEC +
	       (ktime_t)tv->tv_usec * (ktime_t)NS_PER_US;
}

static inline void ktime_to_timeval(ktime_t t, struct timeval *tv)
{
	tv->tv_sec = (__kernel_time_t)(t / (ktime_t)NS_PER_SEC);
	tv->tv_usec = (__kernel_suseconds_t)((t % (ktime_t)NS_PER_SEC) /
					     (ktime_t)NS_PER_US);
}

static inline __s64 ktime_to_sec(ktime_t t)
{
	return t / (ktime_t)NS_PER_SEC;
}

static inline long ktime_to_nsec(ktime_t t)
{
	return (long)(t % (ktime_t)NS_PER_SEC);
}

static inline ktime_t ktime_from_sec_nsec(__s64 sec, long nsec)
{
	return (ktime_t)sec * (ktime_t)NS_PER_SEC + (ktime_t)nsec;
}

static inline ktime_t ktime_realtime(void)
{
	return timekeeper_real();
}

static inline int ktime_set_realtime(ktime_t t)
{
	return timekeeper_set_real(t);
}

static inline time_t ktime_realtime_sec(void)
{
	return (time_t)(ktime_realtime() / (ktime_t)NS_PER_SEC);
}

/*
 * CLOCK_BOOTTIME nanoseconds.  With no suspend support this equals
 * CLOCK_MONOTONIC and is not affected by wall-clock changes.
 */
static inline u64 ktime_boottime_ns(void)
{
	return ktime_get_boottime_ns();
}

/*
 * Sleep for @dur nanoseconds.  On -EINTR, @rem receives the remaining time
 * when non-NULL.  @rem is also zeroed on success.
 */
static inline int ktime_nanosleep(ktime_t dur, ktime_t *rem)
{
	return timekeeper_nanosleep(dur, rem);
}

#endif
