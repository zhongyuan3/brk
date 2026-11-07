#define pr_fmt(fmt) "rtc: " fmt

#include <arch/pgtable.h>
#include <brk/device.h>
#include <brk/goldfish_rtc.h>
#include <brk/initcall.h>
#include <brk/ioremap.h>
#include <brk/kernel.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/of.h>
#include <brk/panic.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/resource.h>
#include <brk/rtc.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <inttypes.h>
#include <uapi/errno.h>

/* Android Goldfish RTC (QEMU virt), see QEMU hw/rtc/goldfish_rtc.c */
#define GOLDFISH_RTC_TIME_LOW 0x00u
#define GOLDFISH_RTC_TIME_HIGH 0x04u

static bool rtc_ok;
static struct goldfish_rtc goldfish_rtc = {
	.lock = SPINLOCK_INITIALIZER("goldfish_rtc"),
};

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

u64 rtc_read_ns(void)
{
	return goldfish_rtc_read_ns(&goldfish_rtc);
}

void rtc_set_ns(u64 ns)
{
	goldfish_rtc_set_ns(&goldfish_rtc, ns);
}

bool rtc_is_available(void)
{
	return rtc_ok;
}

static int goldfish_rtc_setup(u64 phys_base, size_t size)
{
	if (rtc_ok) {
		if (goldfish_rtc.phys_base == phys_base)
			return 0;
		return -EBUSY;
	}

	goldfish_rtc.mmio =
		(u8 volatile *)ioremap(phys_base, size, PTE_R | PTE_W);
	if (!goldfish_rtc.mmio)
		return -ENOMEM;

	goldfish_rtc.phys_base = phys_base;
	goldfish_rtc.size = size;
	rtc_ok = true;

	pr_info("goldfish: mmio=%#" PRIx64 ",ioremap=%p\n", phys_base,
		goldfish_rtc.mmio);
	return 0;
}

int goldfish_rtc_early_probe(void)
{
	struct device_node *node;
	struct resource res;
	int err;

	/*
	 * The device_node tree is normally built by a core initcall, but the
	 * RTC is needed by timekeeper_init() before initcalls run.  of_init()
	 * is idempotent, so the later core initcall becomes a no-op.
	 */
	err = of_init();
	if (err)
		return err;

	node = of_find_compatible_node("google,goldfish-rtc");
	if (!node)
		return -ENODEV;

	err = of_address_to_resource(node, 0, &res);
	if (err)
		return err;

	return goldfish_rtc_setup(res.start, resource_size(&res));
}

static int goldfish_rtc_probe(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *mem;
	int err;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	err = goldfish_rtc_setup(mem->start, resource_size(mem));
	if (err)
		return err;

	dev->driver_data = &goldfish_rtc;
	pr_debug("bound %s at %#" PRIx64 "\n", dev->name, mem->start);
	return 0;
}

static void goldfish_rtc_remove(struct device *dev)
{
	/*
	 * The early wall-clock provider keeps using the mapping for the
	 * lifetime of the kernel; only drop the platform binding here.
	 */
	dev->driver_data = NULL;
}

static const struct device_id goldfish_rtc_ids[] = {
	{ .compatible = "google,goldfish-rtc" },
	{},
};

static struct platform_driver goldfish_rtc_platform_driver = {
	.driver = {
		.name = "goldfish_rtc",
		.id_table = goldfish_rtc_ids,
		.probe = goldfish_rtc_probe,
		.remove = goldfish_rtc_remove,
	},
};

static void goldfish_rtc_driver_init(void)
{
	int err = platform_driver_register(&goldfish_rtc_platform_driver);

	if (err)
		panic("%s(): platform_driver_register failed: %d\n", __func__,
		      err);
}
subsys_initcall(goldfish_rtc_driver_init);
