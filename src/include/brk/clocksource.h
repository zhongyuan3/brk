#ifndef BRK_CLOCKSOURCE_H
#define BRK_CLOCKSOURCE_H

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
int clocksource_init(void);

/* Monotonic nanoseconds from the selected clocksource. */
u64 clocksource_read_ns(void);
u32 clocksource_get_frequency(void);

#endif
