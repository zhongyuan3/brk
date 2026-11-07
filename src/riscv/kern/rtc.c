#define pr_fmt(fmt) "rtc: " fmt

#include <brk/goldfish_rtc.h>
#include <brk/printk.h>
#include <brk/rtc.h>

/*
 * Early wall-clock exception: timekeeper_init() needs an RTC before
 * do_initcalls() runs, so the RTC is brought up directly from the OF tree
 * here.  The platform driver binds to the same device later and adopts the
 * already-initialised controller.
 */
void rtc_init(void)
{
	int err = goldfish_rtc_early_probe();

	if (err)
		pr_warn("no goldfish RTC in device tree (%d)\n", err);
}
