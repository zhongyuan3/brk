#ifndef BRK_TIMEKEEPER_H
#define BRK_TIMEKEEPER_H

#include <brk/lib/types.h>
#include <uapi/time.h>

void timekeeper_init(void);
void timekeeper_tick(void);

u64 jiffies_get(void);
void *timekeeper_wait_chan(void);

void timekeeper_get_mono_ts(struct timespec *ts);
void timekeeper_get_real_ts(struct timespec *ts);
void timekeeper_set_real_ts(const struct timespec *ts);

u64 timekeeper_nanosleep(const struct timespec *dur, struct timespec *rem);

#endif
