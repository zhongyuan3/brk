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

/* Selected clocksource, or NULL before clocksource_init(). */
struct clocksource *clocksource_get_current(void);
u32 clocksource_get_frequency(void);
u64 clocksource_get_resolution_ns(void);

#endif
