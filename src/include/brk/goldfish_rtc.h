#ifndef BRK_GOLDFISH_RTC_H
#define BRK_GOLDFISH_RTC_H

#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <stddef.h>

struct goldfish_rtc {
	u8 volatile *mmio;
	u64 phys_base;
	size_t size;
	spinlock_t lock;
};

u64 goldfish_rtc_read_ns(struct goldfish_rtc *rtc);
void goldfish_rtc_set_ns(struct goldfish_rtc *rtc, u64 ns);

/*
 * Early wall-clock bring-up.  Called before initcalls because timekeeper
 * needs a wall-clock provider before the platform driver model is available.
 */
int goldfish_rtc_early_probe(void);

#endif
