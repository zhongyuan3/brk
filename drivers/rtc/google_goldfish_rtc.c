#include <arch/pgtable.h>
#include <brk/drivers/rtc.h>
#include <brk/kernel/dtb.h>
#include <brk/kernel/printk.h>
#include <brk/kernel/timeconst.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/ioremap.h>
#include <brk/mm/mm.h>
#include <brk/mm/mmio.h>
#include <uapi/time.h>

/* Android Goldfish RTC (QEMU virt), see QEMU hw/rtc/goldfish_rtc.c */
#define RTC_TIME_LOW 0x00u
#define RTC_TIME_HIGH 0x04u

static u8 volatile *rtc_mmio;
static bool rtc_ok;
static SPINLOCK_DEFINE(rtc_lock);

static u64 rtc_read_time_locked(void)
{
	u32 lo;
	u32 hi;

	lo = readl(rtc_mmio + RTC_TIME_LOW);
	hi = readl(rtc_mmio + RTC_TIME_HIGH);
	return ((u64)hi << 32) | (u64)lo;
}

u64 rtc_read_ns(void)
{
	u64 ns = 0;
	spinlock_acquire(&rtc_lock);
	if (rtc_ok)
		ns = rtc_read_time_locked();
	spinlock_release(&rtc_lock);
	return ns;
}

bool rtc_is_available(void)
{
	spinlock_acquire(&rtc_lock);
	bool ok = rtc_ok;
	spinlock_release(&rtc_lock);
	return ok;
}

void rtc_read_timeval(struct timeval *tv)
{
	u64 ns;

	spinlock_acquire(&rtc_lock);
	if (rtc_ok) {
		ns = rtc_read_time_locked();
		tv->tv_sec = ns / NS_PER_SEC;
		tv->tv_usec = ns % NS_PER_SEC / NS_PER_US;
	}
	spinlock_release(&rtc_lock);
}

void rtc_set_timeval(const struct timeval *tv)
{
	u64 ns;

	spinlock_acquire(&rtc_lock);
	if (rtc_ok) {
		ns = (u64)tv->tv_sec * NS_PER_SEC +
		     (u64)tv->tv_usec * NS_PER_US;

		/* Same order as Linux drivers/rtc/rtc-goldfish.c */
		writel((u32)(ns >> 32), rtc_mmio + RTC_TIME_HIGH);
		writel((u32)ns, rtc_mmio + RTC_TIME_LOW);
	}
	spinlock_release(&rtc_lock);
}

void rtc_read_timespec(struct timespec *ts)
{
	u64 ns;

	spinlock_acquire(&rtc_lock);
	if (rtc_ok) {
		ns = rtc_read_time_locked();
		ts->tv_sec = ns / NS_PER_SEC;
		ts->tv_nsec = ns % NS_PER_SEC;
	}
	spinlock_release(&rtc_lock);
}

void rtc_set_timespec(const struct timespec *ts)
{
	u64 ns;

	spinlock_acquire(&rtc_lock);
	if (rtc_ok) {
		ns = (u64)ts->tv_sec * NS_PER_SEC + (u64)ts->tv_nsec;

		/* Same order as Linux drivers/rtc/rtc-goldfish.c */
		writel((u32)(ns >> 32), rtc_mmio + RTC_TIME_HIGH);
		writel((u32)ns, rtc_mmio + RTC_TIME_LOW);
	}
	spinlock_release(&rtc_lock);
}

void rtc_init(void)
{
	struct rtc_device dev;
	int err;

	err = dtb_parse_rtc(&dev);
	if (err) {
		klog_warn("rtc: no goldfish RTC in device tree (%d)\n", err);
		return;
	}

	rtc_mmio =
		(u8 volatile *)ioremap(dev.phys_base, dev.size, PTE_R | PTE_W);
	if (!rtc_mmio) {
		klog_warn("rtc: ioremap failed for phys %#lx\n", dev.phys_base);
		return;
	}

	rtc_ok = true;
}
