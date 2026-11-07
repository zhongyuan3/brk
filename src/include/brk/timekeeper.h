#ifndef BRK_TIMEKEEPER_H
#define BRK_TIMEKEEPER_H

#include <brk/types.h>
#include <stdbool.h>

struct wall_clock;

struct wall_clock_ops {
	int (*read_ns)(struct wall_clock *wc, u64 *ns);
	int (*set_ns)(struct wall_clock *wc, u64 ns);
};

/*
 * A wall-clock provider is normally an RTC.  The provider is registered from
 * its platform driver probe; timekeeper applies the first provider's time to
 * the wall offset and later writes back through set_ns().
 */
struct wall_clock {
	const char *name;
	const struct wall_clock_ops *ops;
};

void timekeeper_init(void);
void timekeeper_tick(void);
bool timekeeper_initialized(void);

u64 jiffies_get(void);
u64 timekeeper_mono_ns(void);
void *timekeeper_wait_chan(void);

ktime_t timekeeper_real(void);
int timekeeper_set_real(ktime_t ns);

int timekeeper_register_wall_clock(struct wall_clock *wc);
void timekeeper_unregister_wall_clock(struct wall_clock *wc);

int timekeeper_nanosleep(ktime_t dur, ktime_t *rem);

#endif
