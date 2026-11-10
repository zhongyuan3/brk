#define pr_fmt(fmt) "platform: " fmt
#include <brk/device.h>
#include <brk/devtmpfs.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/of.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <stdbool.h>
#include <uapi/errno.h>

struct bus_type platform_bus = {
	.name = "platform",
};

static bool platform_match(struct device *dev, struct device_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	const struct device_id *id;

	if (!drv->id_table)
		return false;

	if (dev->of_node) {
		for (id = drv->id_table; id->compatible; ++id) {
			if (of_device_is_compatible(dev->of_node,
						    id->compatible))
				return true;
		}
	}

	if (!pdev->compatible)
		return false;

	for (id = drv->id_table; id->compatible; ++id) {
		if (!strcmp(id->compatible, pdev->compatible))
			return true;
	}
	return false;
}

void platform_bus_init(void)
{
	platform_bus.match = platform_match;
	bus_register(&platform_bus);
}

void platform_device_release(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

	kfree(pdev->resources);
	kfree(pdev);
}

int platform_device_register_simple(const char *name, const char *compatible,
				    u64 phys_base, u64 size, u32 irq,
				    void *pdata)
{
	struct platform_device *pdev;
	unsigned int nr = 0;
	int err;

	if (!name || !compatible)
		return -EINVAL;

	pdev = kzalloc(sizeof(*pdev));
	if (!pdev)
		return -ENOMEM;

	if (size)
		nr++;
	if (irq)
		nr++;
	if (nr) {
		pdev->resources = kcalloc(nr, sizeof(*pdev->resources));
		if (!pdev->resources) {
			kfree(pdev);
			return -ENOMEM;
		}
	}

	nr = 0;
	if (size) {
		pdev->resources[nr].start = phys_base;
		pdev->resources[nr].end = phys_base + size - 1;
		pdev->resources[nr].flags = IORESOURCE_MEM;
		pdev->resources[nr].name = name;
		nr++;
	}
	if (irq) {
		pdev->resources[nr].start = irq;
		pdev->resources[nr].end = irq;
		pdev->resources[nr].flags = IORESOURCE_IRQ;
		pdev->resources[nr].name = name;
		nr++;
	}
	pdev->num_resources = nr;

	device_init(&pdev->dev, name);
	pdev->dev.bus = &platform_bus;
	pdev->dev.release = platform_device_release;
	pdev->compatible = compatible;
	pdev->pdata = pdata;

	err = device_register(&pdev->dev);
	if (err) {
		device_put(&pdev->dev);
		return err;
	}

	return 0;
}

void platform_device_unregister(struct platform_device *pdev)
{
	if (!pdev)
		return;
	device_unregister(&pdev->dev);
}

struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index)
{
	if (!pdev)
		return NULL;

	for (unsigned int i = 0; i < pdev->num_resources; i++) {
		struct resource *res = &pdev->resources[i];

		if (type && !(res->flags & type))
			continue;
		if (index-- == 0)
			return res;
	}
	return NULL;
}

int platform_get_irq(struct platform_device *pdev, unsigned int index)
{
	struct resource *res;

	if (!pdev)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, index);
	if (res)
		return (int)res->start;

	/*
	 * OF devices carry no IRQ resource: resolve interrupts from the node
	 * at probe time, after the IRQ domain is available.
	 */
	if (pdev->dev.of_node)
		return of_irq_get(pdev->dev.of_node, (int)index);

	return -ENOENT;
}

int platform_driver_register(struct platform_driver *pdrv)
{
	if (!pdrv)
		return -EINVAL;

	pdrv->driver.bus = &platform_bus;
	return driver_register(&pdrv->driver);
}

void platform_driver_unregister(struct platform_driver *pdrv)
{
	if (!pdrv)
		return;
	driver_unregister(&pdrv->driver);
}
