#ifndef BRK_TIMEKEEPER_H
#define BRK_TIMEKEEPER_H

#include <brk/types.h>

struct wall_clock;

struct wall_clock_ops {
	int (*read_ns)(struct wall_clock *wc, u64 *ns);

	/*
	 * Called with the timekeeper lock held (interrupts disabled).  It must
	 * not sleep or call back into timekeeper.
	 */
	int (*set_ns)(struct wall_clock *wc, u64 ns);
};

/*
 * A wall-clock provider is normally an RTC.  The provider is registered from
 * its platform driver probe; timekeeper applies the first provider's time to
 * the realtime offset and later writes back through set_ns().
 */
struct wall_clock {
	const char *name;
	const struct wall_clock_ops *ops;
};

void timekeeper_init(void);

/* Advance the timekeeper base; called from the periodic tick. */
void timekeeping_advance(void);

/* CLOCK_MONOTONIC nanoseconds since timekeeper initialization. */
u64 ktime_get_mono_ns(void);

/* CLOCK_BOOTTIME; equals monotonic time while suspend is unsupported. */
u64 ktime_get_boottime_ns(void);

ktime_t timekeeper_real(void);
int timekeeper_set_real(ktime_t ns);

int timekeeper_register_wall_clock(struct wall_clock *wc);
void timekeeper_unregister_wall_clock(struct wall_clock *wc);

int timekeeper_nanosleep(ktime_t dur, ktime_t *rem);

#endif
