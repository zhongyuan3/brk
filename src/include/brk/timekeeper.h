#ifndef BRK_TIMEKEEPER_H
#define BRK_TIMEKEEPER_H

#include <brk/types.h>

void timekeeper_init(void);
void timekeeper_tick(void);
bool timekeeper_initialized(void);

u64 jiffies_get(void);
u64 timekeeper_mono_ns(void);
void *timekeeper_wait_chan(void);

ktime_t timekeeper_real(void);
void timekeeper_set_real(ktime_t ns);

int timekeeper_nanosleep(ktime_t dur, ktime_t *rem);

#endif
