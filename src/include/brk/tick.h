#ifndef BRK_TICK_H
#define BRK_TICK_H

#include <brk/list.h>
#include <brk/types.h>
#include <stdbool.h>

struct clock_event_device;

/*
 * Periodic work driven by the boot hart's clock event device.  Handlers run
 * in interrupt context, must not sleep, and must not register/unregister
 * handlers from inside the callback.
 */
struct tick_handler {
	void (*fn)(void);
	struct list_head node;
	bool registered;
};

void tick_init(void);
void tick_handle_periodic(struct clock_event_device *dev);

u64 tick_get_jiffies(void);

int tick_register_handler(struct tick_handler *th);
void tick_unregister_handler(struct tick_handler *th);

#endif
