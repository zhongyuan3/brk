#include <arch/pgtable.h>
#include <brk/drivers/of.h>
#include <brk/drivers/rtc.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/ioremap.h>
#include <brk/mm/mm.h>
#include <brk/mm/mmio.h>
#include <brk/printk/printk.h>
#include <brk/time/timeconst.h>
#include <uapi/brk/time.h>

/* Android Goldfish RTC (QEMU virt), see QEMU hw/rtc/goldfish_rtc.c */
#define RTC_TIME_LOW 0x00u
#define RTC_TIME_HIGH 0x04u

static uint8_t volatile *rtc_mmio;
static bool rtc_ok;
static SPINLOCK_DEFINE(rtc_lock);

static uint64_t rtc_read_time_locked(void)
{
	uint32_t lo;
	uint32_t hi;

	lo = readl(rtc_mmio + RTC_TIME_LOW);
	hi = readl(rtc_mmio + RTC_TIME_HIGH);
	return ((uint64_t)hi << 32) | (uint64_t)lo;
}

uint64_t rtc_read_ns(void)
{
	uint64_t ns = 0;
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
	uint64_t ns;

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
	uint64_t ns;

	spinlock_acquire(&rtc_lock);
	if (rtc_ok) {
		ns = (uint64_t)tv->tv_sec * NS_PER_SEC +
		     (uint64_t)tv->tv_usec * NS_PER_US;

		/* Same order as Linux drivers/rtc/rtc-goldfish.c */
		writel((uint32_t)(ns >> 32), rtc_mmio + RTC_TIME_HIGH);
		writel((uint32_t)ns, rtc_mmio + RTC_TIME_LOW);
	}
	spinlock_release(&rtc_lock);
}

void rtc_read_timespec(struct timespec *ts)
{
	uint64_t ns;

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
	uint64_t ns;

	spinlock_acquire(&rtc_lock);
	if (rtc_ok) {
		ns = (uint64_t)ts->tv_sec * NS_PER_SEC + (uint64_t)ts->tv_nsec;

		/* Same order as Linux drivers/rtc/rtc-goldfish.c */
		writel((uint32_t)(ns >> 32), rtc_mmio + RTC_TIME_HIGH);
		writel((uint32_t)ns, rtc_mmio + RTC_TIME_LOW);
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

	rtc_mmio = (uint8_t volatile *)ioremap(dev.phys_base, dev.size,
					       PTE_R | PTE_W);
	if (!rtc_mmio) {
		klog_warn("rtc: ioremap failed for phys %#lx\n", dev.phys_base);
		return;
	}

	rtc_ok = true;
}
