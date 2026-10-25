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

static u64 goldfish_rtc_read_time_locked(struct goldfish_rtc *rtc)
{
	u32 lo;
	u32 hi;

	lo = readl(rtc->mmio + GOLDFISH_RTC_TIME_LOW);
	hi = readl(rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	return ((u64)hi << 32) | (u64)lo;
}

u64 goldfish_rtc_read_ns(struct goldfish_rtc *rtc)
{
	u64 ns = 0;
	spinlock_acquire(&rtc->lock);
	ns = goldfish_rtc_read_time_locked(rtc);
	spinlock_release(&rtc->lock);
	return ns;
}

void goldfish_rtc_read_timeval(struct goldfish_rtc *rtc, struct timeval *tv)
{
	u64 ns;

	spinlock_acquire(&rtc->lock);

	ns = goldfish_rtc_read_time_locked(rtc);
	tv->tv_sec = ns / NS_PER_SEC;
	tv->tv_usec = ns % NS_PER_SEC / NS_PER_US;

	spinlock_release(&rtc->lock);
}

void goldfish_rtc_set_timeval(struct goldfish_rtc *rtc,
			      const struct timeval *tv)
{
	u64 ns;

	spinlock_acquire(&rtc->lock);

	ns = (u64)tv->tv_sec * NS_PER_SEC + (u64)tv->tv_usec * NS_PER_US;

	/* Same order as Linux drivers/rtc/rtc-goldfish.c */
	writel((u32)(ns >> 32), rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	writel((u32)ns, rtc->mmio + GOLDFISH_RTC_TIME_LOW);

	spinlock_release(&rtc->lock);
}

void goldfish_rtc_read_timespec(struct goldfish_rtc *rtc, struct timespec *ts)
{
	u64 ns;

	spinlock_acquire(&rtc->lock);

	ns = goldfish_rtc_read_time_locked(rtc);
	ts->tv_sec = ns / NS_PER_SEC;
	ts->tv_nsec = ns % NS_PER_SEC;

	spinlock_release(&rtc->lock);
}

void goldfish_rtc_set_timespec(struct goldfish_rtc *rtc,
			       const struct timespec *ts)
{
	u64 ns;

	spinlock_acquire(&rtc->lock);

	ns = (u64)ts->tv_sec * NS_PER_SEC + (u64)ts->tv_nsec;

	/* Same order as Linux drivers/rtc/rtc-goldfish.c */
	writel((u32)(ns >> 32), rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	writel((u32)ns, rtc->mmio + GOLDFISH_RTC_TIME_LOW);

	spinlock_release(&rtc->lock);
}
