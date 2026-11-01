#ifndef BRK_RTC_H
#define BRK_RTC_H

#include <brk/types.h>

struct rtc_device {
	u64 phys_base;
	size_t size;
};

void rtc_init(void);
bool rtc_is_available(void);

/* Wall-clock time in nanoseconds since the epoch. */
u64 rtc_read_ns(void);
void rtc_set_ns(u64 ns);

#endif
