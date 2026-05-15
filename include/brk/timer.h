#ifndef BRK_TIMER_H
#define BRK_TIMER_H

#include <brk/time.h>
#include <brk/types.h>

void timer_init(void);
u64 timer_get_time(void);
void timer_set_next(void);
void timer_handle_int(void);
void timer_srand(void);
u32 timer_rand(void);

void walltime_get(struct timeval *tv);
void walltime_set(const struct timeval *tv);

void walltime_get_ts(struct timespec *ts);
void walltime_set_ts(const struct timespec *ts);

u64 do_nanosleep(const struct timespec *dur, struct timespec *rem);

u64 jiffies_get(void);

void boot_time_get_ts(struct timespec *ts);

#endif
