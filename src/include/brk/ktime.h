#ifndef BRK_KTIME_H
#define BRK_KTIME_H

#include <brk/stat_types.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <uapi/time.h>

struct fs_inode;

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

ktime_t ktime_get_real(void);
int ktime_set_real(ktime_t t);
time_t ktime_get_real_sec(void);

/*
 * Monotonic nanoseconds since timekeeper initialization.  This is used for
 * CLOCK_MONOTONIC/CLOCK_BOOTTIME and is not affected by wall-clock changes.
 */
u64 ktime_get_boot_ns(void);

/*
 * Sleep for @dur nanoseconds.  On -EINTR, @rem receives the remaining time
 * when non-NULL.  @rem is also zeroed on success.
 */
int ktime_nanosleep(ktime_t dur, ktime_t *rem);

void inode_times_set_all_now(struct fs_inode *inode);
void inode_times_set(struct fs_inode *inode, const ktime_t *atime,
		     const ktime_t *mtime);
void inode_touch_mtime(struct fs_inode *inode);
void inode_touch_ctime(struct fs_inode *inode);
void inode_touch_mtime_ctime(struct fs_inode *inode);

void inode_times_to_kstat(struct fs_inode *inode, struct kstat *ks);

#endif
