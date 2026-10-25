#include <arch/pgtable.h>
#include <brk/dt.h>
#include <brk/goldfish_rtc.h>
#include <brk/ioremap.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/printk.h>
#include <brk/rtc.h>
#include <brk/spinlock.h>
#include <brk/spinlock_types.h>
#include <brk/timeconst.h>
#include <uapi/time.h>

/* Android Goldfish RTC (QEMU virt), see QEMU hw/rtc/goldfish_rtc.c */
#define RTC_TIME_LOW 0x00u
#define RTC_TIME_HIGH 0x04u

static bool rtc_ok;
static struct goldfish_rtc rtc = {
	.lock = SPINLOCK_INITIALIZER("goldfish_rtc"),
};

u64 rtc_read_ns(void)
{
	return goldfish_rtc_read_ns(&rtc);
}

bool rtc_is_available(void)
{
	return rtc_ok;
}

void rtc_read_timeval(struct timeval *tv)
{
	goldfish_rtc_read_timeval(&rtc, tv);
}

void rtc_set_timeval(const struct timeval *tv)
{
	goldfish_rtc_set_timeval(&rtc, tv);
}

void rtc_read_timespec(struct timespec *ts)
{
	goldfish_rtc_read_timespec(&rtc, ts);
}

void rtc_set_timespec(const struct timespec *ts)
{
	goldfish_rtc_set_timespec(&rtc, ts);
}

void rtc_init(void)
{
	struct rtc_device dev;
	int err;

	err = dt_parse_rtc(&dev);
	if (err) {
		klog_warn("rtc: no goldfish RTC in device tree (%d)\n", err);
		return;
	}

	rtc.mmio =
		(u8 volatile *)ioremap(dev.phys_base, dev.size, PTE_R | PTE_W);
	if (!rtc.mmio) {
		klog_warn("rtc: ioremap failed for phys %#" PRIx64 "\n",
			  dev.phys_base);
		return;
	}

	rtc_ok = true;

	klog_info("rtc: goldfish RTC initialized: mmio=%#lx, ioremap=%p\n",
		  dev.phys_base, rtc.mmio);
}
