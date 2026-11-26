#ifndef _BRK_CLOCKSOURCE_H
#define _BRK_CLOCKSOURCE_H

#include <brk/list.h>
#include <brk/types.h>

struct clocksource {
	const char *name;
	u64 (*read)(void);
	u32 frequency;
	u32 rating;
	struct list_head list;
};

int clocksource_register(struct clocksource *cs);
struct clocksource *clocksource_select(void);
void clocksource_init(void);

/* Selected clocksource, or NULL before clocksource_init(). */
struct clocksource *clocksource_get_current(void);

/* Shared, overflow-conscious cycle <-> nanosecond conversions. */
u64 clocksource_cycles_to_ns(const struct clocksource *cs, u64 cycles);
u64 clocksource_ns_to_cycles(const struct clocksource *cs, u64 ns);
u64 clocksource_ns_to_cycles_freq(u32 frequency, u64 ns);
u32 clocksource_get_frequency(void);
u64 clocksource_get_resolution_ns(void);

#endif
