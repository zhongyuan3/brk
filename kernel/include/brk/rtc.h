#ifndef BRK_RTC_H
#define BRK_RTC_H

#include <brk/types.h>
#include <uapi/brk/time.h>

struct rtc_device {
	uint64_t phys_base;
	size_t size;
	uint32_t irq;
};

void rtc_init(void);
uint64_t rtc_read_ns(void);
void rtc_read_timeval(struct timeval *tv);
void rtc_set_timeval(const struct timeval *tv);
void rtc_read_timespec(struct timespec *ts);
void rtc_set_timespec(const struct timespec *ts);
bool rtc_is_available(void);

#endif
