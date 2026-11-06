#include <arch/pgtable.h>
#include <brk/dt.h>
#include <brk/goldfish_rtc.h>
#include <brk/ioremap.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/printk.h>
#include <brk/rtc.h>
#include <brk/spinlock.h>
#include <brk/types.h>

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
	u64 ns;

	spin_lock(&rtc->lock);
	ns = goldfish_rtc_read_time_locked(rtc);
	spin_unlock(&rtc->lock);
	return ns;
}

void goldfish_rtc_set_ns(struct goldfish_rtc *rtc, u64 ns)
{
	spin_lock(&rtc->lock);

	/* Same order as Linux drivers/rtc/rtc-goldfish.c */
	writel((u32)(ns >> 32), rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	writel((u32)ns, rtc->mmio + GOLDFISH_RTC_TIME_LOW);

	spin_unlock(&rtc->lock);
}
