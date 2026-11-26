#ifndef _BRK_TICK_H
#define _BRK_TICK_H

#include <brk/compiler_attr.h>
#include <brk/list.h>
#include <brk/types.h>
#include <stdbool.h>

struct clock_event_device;

/*
 * Per-CPU periodic work.  Each CPU has its own tick device and its own
 * handler list; handlers are registered on the current CPU and run only
 * from that CPU's tick interrupt.
 *
 * Handlers run in interrupt context, must not sleep, and must not
 * register/unregister handlers from inside the callback.
 */
struct tick_handler {
	void (*fn)(void);
	struct list_head node;
	cpuid_t cpu;
	bool registered;
};

void tick_init(void);
void tick_init_cpu(void);
void tick_handle_irq(void);
void tick_handle_periodic(struct clock_event_device *dev);
void tick_reschedule(void);

int tick_register_handler(struct tick_handler *th);
void tick_unregister_handler(struct tick_handler *th);

#endif
