#ifndef BRK_GOLDFISH_RTC_H
#define BRK_GOLDFISH_RTC_H

#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <uapi/time.h>

struct goldfish_rtc {
	u8 volatile *mmio;
	spinlock_t lock;
};

u64 goldfish_rtc_read_ns(struct goldfish_rtc *rtc);
void goldfish_rtc_read_timeval(struct goldfish_rtc *rtc, struct timeval *tv);
void goldfish_rtc_set_timeval(struct goldfish_rtc *rtc,
			      const struct timeval *tv);
void goldfish_rtc_read_timespec(struct goldfish_rtc *rtc, struct timespec *ts);
void goldfish_rtc_set_timespec(struct goldfish_rtc *rtc,
			       const struct timespec *ts);

#endif
