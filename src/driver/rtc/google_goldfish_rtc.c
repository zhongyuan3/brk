#include <arch/pgtable.h>
#include <brk/dt.h>
#include <brk/goldfish_rtc.h>
#include <brk/ioremap.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/printk.h>
#include <brk/rtc.h>
#include <brk/spinlock.h>
#include <brk/timeconst.h>
#include <uapi/time.h>

/* Android Goldfish RTC (QEMU virt), see QEMU hw/rtc/goldfish_rtc.c */
#define GOLDFISH_RTC_TIME_LOW 0x00u
#define GOLDFISH_RTC_TIME_HIGH 0x04u

static uint64_t goldfish_rtc_read_time_locked(struct goldfish_rtc *rtc)
{
	uint32_t lo;
	uint32_t hi;

	lo = readl(rtc->mmio + GOLDFISH_RTC_TIME_LOW);
	hi = readl(rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	return ((uint64_t)hi << 32) | (uint64_t)lo;
}

uint64_t goldfish_rtc_read_ns(struct goldfish_rtc *rtc)
{
	uint64_t ns = 0;
	spin_lock(&rtc->lock);
	ns = goldfish_rtc_read_time_locked(rtc);
	spin_unlock(&rtc->lock);
	return ns;
}

void goldfish_rtc_read_timeval(struct goldfish_rtc *rtc, struct timeval *tv)
{
	uint64_t ns;

	spin_lock(&rtc->lock);

	ns = goldfish_rtc_read_time_locked(rtc);
	tv->tv_sec = ns / NS_PER_SEC;
	tv->tv_usec = ns % NS_PER_SEC / NS_PER_US;

	spin_unlock(&rtc->lock);
}

void goldfish_rtc_set_timeval(struct goldfish_rtc *rtc,
			      const struct timeval *tv)
{
	uint64_t ns;

	spin_lock(&rtc->lock);

	ns = (uint64_t)tv->tv_sec * NS_PER_SEC +
	     (uint64_t)tv->tv_usec * NS_PER_US;

	/* Same order as Linux drivers/rtc/rtc-goldfish.c */
	writel((uint32_t)(ns >> 32), rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	writel((uint32_t)ns, rtc->mmio + GOLDFISH_RTC_TIME_LOW);

	spin_unlock(&rtc->lock);
}

void goldfish_rtc_read_timespec(struct goldfish_rtc *rtc, struct timespec *ts)
{
	uint64_t ns;

	spin_lock(&rtc->lock);

	ns = goldfish_rtc_read_time_locked(rtc);
	ts->tv_sec = ns / NS_PER_SEC;
	ts->tv_nsec = ns % NS_PER_SEC;

	spin_unlock(&rtc->lock);
}

void goldfish_rtc_set_timespec(struct goldfish_rtc *rtc,
			       const struct timespec *ts)
{
	uint64_t ns;

	spin_lock(&rtc->lock);

	ns = (uint64_t)ts->tv_sec * NS_PER_SEC + (uint64_t)ts->tv_nsec;

	/* Same order as Linux drivers/rtc/rtc-goldfish.c */
	writel((uint32_t)(ns >> 32), rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	writel((uint32_t)ns, rtc->mmio + GOLDFISH_RTC_TIME_LOW);

	spin_unlock(&rtc->lock);
}
