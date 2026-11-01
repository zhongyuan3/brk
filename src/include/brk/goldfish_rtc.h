#ifndef BRK_GOLDFISH_RTC_H
#define BRK_GOLDFISH_RTC_H

#include <brk/spinlock_types.h>
#include <brk/types.h>

struct goldfish_rtc {
	u8 volatile *mmio;
	spinlock_t lock;
};

u64 goldfish_rtc_read_ns(struct goldfish_rtc *rtc);
void goldfish_rtc_set_ns(struct goldfish_rtc *rtc, u64 ns);

#endif
