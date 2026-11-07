#define pr_fmt(fmt) "rtc: " fmt
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
#include <inttypes.h>

/* Android Goldfish RTC lives behind a two-register MMIO interface. */

static bool rtc_ok;
static struct goldfish_rtc rtc = {
	.lock = SPINLOCK_INITIALIZER("goldfish_rtc"),
};

u64 rtc_read_ns(void)
{
	return goldfish_rtc_read_ns(&rtc);
}

void rtc_set_ns(u64 ns)
{
	goldfish_rtc_set_ns(&rtc, ns);
}

bool rtc_is_available(void)
{
	return rtc_ok;
}

void rtc_init(void)
{
	struct rtc_device dev;
	int err;

	err = dt_parse_rtc(&dev);
	if (err) {
		pr_warn("no goldfish RTC in device tree (%d)\n", err);
		return;
	}

	rtc.mmio =
		(u8 volatile *)ioremap(dev.phys_base, dev.size, PTE_R | PTE_W);
	if (!rtc.mmio) {
		pr_warn("ioremap failed for phys %#" PRIx64 "\n",
			dev.phys_base);
		return;
	}

	rtc_ok = true;

	pr_info("goldfish: mmio=%#lx,ioremap=%p\n", dev.phys_base, rtc.mmio);
}
