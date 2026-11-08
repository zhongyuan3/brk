#ifndef BRK_CLOCKEVENT_H
#define BRK_CLOCKEVENT_H

#include <brk/list.h>
#include <brk/types.h>
#include <stdbool.h>

enum clock_event_mode {
	CLOCK_EVT_MODE_UNUSED = 0,
	CLOCK_EVT_MODE_SHUTDOWN,
	CLOCK_EVT_MODE_PERIODIC,
	CLOCK_EVT_MODE_ONESHOT,
};

/*
 * A clock event device programs the next timer interrupt.  Expiry times are
 * absolute CLOCK_MONOTONIC nanoseconds; the device converts the resulting
 * delta into its own cycles.
 */
struct clock_event_device {
	const char *name;
	u32 rating;
	u32 frequency;
	u64 min_delta_ns;
	u64 max_delta_ns; /* 0 means no upper bound */

	int (*set_next_event)(u64 delta_ns, struct clock_event_device *dev);
	int (*set_mode)(enum clock_event_mode mode,
			struct clock_event_device *dev);
	void (*event_handler)(struct clock_event_device *dev);

	struct list_head list;
	bool registered;
};

int clockevents_register_device(struct clock_event_device *dev);
struct clock_event_device *clockevents_get_current(void);

int clockevents_set_mode(struct clock_event_device *dev,
			 enum clock_event_mode mode);
int clockevents_program_event(struct clock_event_device *dev, u64 expires_ns);
void clockevents_shutdown(struct clock_event_device *dev);

#endif
