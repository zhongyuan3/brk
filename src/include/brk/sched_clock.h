#ifndef BRK_SCHED_CLOCK_H
#define BRK_SCHED_CLOCK_H

#include <brk/types.h>

struct clocksource;

/*
 * Boot-time clock used by printk (and later by scheduler bookkeeping).  It is
 * separate from CLOCK_MONOTONIC: sched_clock_init() establishes its epoch as
 * soon as a clocksource is selected, and the epoch never moves when
 * timekeeping starts.
 */
void sched_clock_init(struct clocksource *cs);
u64 sched_clock(void);

/* Linux-style wrapper; currently has no irqtime/stability tracking. */
u64 local_clock(void);

#endif
