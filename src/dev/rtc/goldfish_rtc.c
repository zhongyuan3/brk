#define pr_fmt(fmt) "rtc: " fmt
#include <arch/pgtable.h>
#include <brk/container_of.h>
#include <brk/device.h>
#include <brk/initcall.h>
#include <brk/ioremap.h>
#include <brk/kmalloc.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/panic.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/resource.h>
#include <brk/spinlock.h>
#include <brk/timekeeper.h>
#include <brk/types.h>
#include <inttypes.h>
#include <stddef.h>
#include <uapi/errno.h>

/* Android Goldfish RTC (QEMU virt), see QEMU hw/rtc/goldfish_rtc.c */
#define GOLDFISH_RTC_TIME_LOW 0x00u
#define GOLDFISH_RTC_TIME_HIGH 0x04u

struct goldfish_rtc {
	u8 volatile *mmio;
	size_t size;
	spinlock_t lock;
	struct wall_clock wall;
};

static u64 goldfish_rtc_read_time_locked(struct goldfish_rtc *rtc)
{
	u32 lo;
	u32 hi;

	lo = readl(rtc->mmio + GOLDFISH_RTC_TIME_LOW);
	hi = readl(rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	return ((u64)hi << 32) | (u64)lo;
}

static u64 goldfish_rtc_read_ns(struct goldfish_rtc *rtc)
{
	u64 ns;

	spin_lock(&rtc->lock);
	ns = goldfish_rtc_read_time_locked(rtc);
	spin_unlock(&rtc->lock);
	return ns;
}

static void goldfish_rtc_set_ns(struct goldfish_rtc *rtc, u64 ns)
{
	spin_lock(&rtc->lock);

	/* Same order as Linux drivers/rtc/rtc-goldfish.c */
	writel((u32)(ns >> 32), rtc->mmio + GOLDFISH_RTC_TIME_HIGH);
	writel((u32)ns, rtc->mmio + GOLDFISH_RTC_TIME_LOW);

	spin_unlock(&rtc->lock);
}

static int goldfish_rtc_wall_read(struct wall_clock *wc, u64 *ns)
{
	struct goldfish_rtc *rtc = container_of(wc, struct goldfish_rtc, wall);

	if (!ns)
		return -EINVAL;

	*ns = goldfish_rtc_read_ns(rtc);
	return 0;
}

static int goldfish_rtc_wall_set(struct wall_clock *wc, u64 ns)
{
	struct goldfish_rtc *rtc = container_of(wc, struct goldfish_rtc, wall);

	goldfish_rtc_set_ns(rtc, ns);
	return 0;
}

static const struct wall_clock_ops goldfish_rtc_wall_ops = {
	.read_ns = goldfish_rtc_wall_read,
	.set_ns = goldfish_rtc_wall_set,
};

static int goldfish_rtc_probe(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct goldfish_rtc *rtc;
	struct resource *mem;
	int err;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	rtc = kzalloc(sizeof(*rtc));
	if (!rtc)
		return -ENOMEM;

	spinlock_init(&rtc->lock, "goldfish_rtc");
	rtc->size = resource_size(mem);
	rtc->mmio =
		(u8 volatile *)ioremap(mem->start, rtc->size, PTE_R | PTE_W);
	if (!rtc->mmio) {
		kfree(rtc);
		return -ENOMEM;
	}

	rtc->wall.name = "goldfish-rtc";
	rtc->wall.ops = &goldfish_rtc_wall_ops;

	err = timekeeper_register_wall_clock(&rtc->wall);
	if (err) {
		iounmap((void *)rtc->mmio, rtc->size);
		kfree(rtc);
		return err;
	}

	dev->driver_data = rtc;
	pr_info("goldfish: mmio=%#" PRIx64 "\n", mem->start);
	return 0;
}

static void goldfish_rtc_remove(struct device *dev)
{
	struct goldfish_rtc *rtc = dev->driver_data;

	if (!rtc)
		return;

	dev->driver_data = NULL;
	timekeeper_unregister_wall_clock(&rtc->wall);
	iounmap((void *)rtc->mmio, rtc->size);
	kfree(rtc);
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
