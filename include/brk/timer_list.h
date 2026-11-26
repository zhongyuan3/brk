#ifndef _BRK_TIMER_LIST_H
#define _BRK_TIMER_LIST_H

#include <brk/list.h>
#include <brk/types.h>
#include <stdbool.h>

/*
 * Software timer.  Expiry is an absolute CLOCK_MONOTONIC time in nanoseconds.
 * Callbacks run from the tick interrupt and must not sleep.
 */
struct timer_list {
	struct list_head node;
	u64 expires_ns;
	void (*function)(struct timer_list *timer);
	bool pending;
	bool running;
};

void timer_setup(struct timer_list *timer,
		 void (*function)(struct timer_list *timer));
void timer_set_expires(struct timer_list *timer, u64 expires_ns);

int timer_add(struct timer_list *timer);
bool timer_del(struct timer_list *timer);

/*
 * Remove @timer and wait until a callback that has already started returns.
 * The timer must not be re-armed concurrently.  Must not be called from the
 * timer callback itself.
 */
void timer_del_sync(struct timer_list *timer);

bool timer_pending(struct timer_list *timer);

/* Earliest pending expiry, or ~0ULL when the timer list is empty. */
u64 timer_next_expiry_ns(void);

/* Run all timers whose expiry is <= @now_ns.  Called from a CPU tick. */
void timer_run_expired(u64 now_ns);

#endif
