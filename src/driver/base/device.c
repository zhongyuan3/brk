#include <brk/device_model.h>
#include <brk/initcall.h>
#include <brk/devtmpfs.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <uapi/errno.h>

static LIST_DEFINE(bus_list);
static LIST_DEFINE(device_list);
static SPINLOCK_DEFINE(device_lock);

void device_init(struct device *dev, const char *name)
{
	memset(dev, 0, sizeof(*dev));
	if (name)
		strlcpy(dev->name, name, sizeof(dev->name));
	list_init(&dev->bus_list);
	list_init(&dev->device_list);
	list_init(&dev->children);
	list_init(&dev->child_node);
}

int bus_register(struct bus_type *bus)
{
	if (!bus || !bus->name)
		return -EINVAL;

	list_init(&bus->devices);
	list_init(&bus->drivers);
	list_init(&bus->list);

	spin_lock(&device_lock);
	list_add_tail(&bus->list, &bus_list);
	spin_unlock(&device_lock);
	return 0;
}

static void device_probe(struct device *dev)
{
	struct device_driver *drv;
	int err;

	if (!dev->bus || !dev->bus->match)
		return;

	spin_lock(&device_lock);
	if (dev->driver) {
		spin_unlock(&device_lock);
		return;
	}

	list_for_each_entry(drv, &dev->bus->drivers, bus_list) {
		if (!dev->bus->match(dev, drv))
			continue;

		/* Claim the device before dropping the lock. */
		dev->driver = drv;
		spin_unlock(&device_lock);

		err = drv->probe ? drv->probe(dev) : 0;
		if (err) {
			pr_warn("device: %s: probe by %s failed: %d\n",
				dev->name, drv->name, err);
			spin_lock(&device_lock);
			if (dev->driver == drv)
				dev->driver = NULL;
			spin_unlock(&device_lock);
		}
		return;
	}

	spin_unlock(&device_lock);
}

int device_register(struct device *dev)
{
	if (!dev || !dev->name[0])
		return -EINVAL;

	spin_lock(&device_lock);
	if (dev->registered) {
		spin_unlock(&device_lock);
		return -EBUSY;
	}

	dev->registered = true;
	list_add_tail(&dev->device_list, &device_list);
	if (dev->parent)
		list_add_tail(&dev->child_node, &dev->parent->children);
	if (dev->bus)
		list_add_tail(&dev->bus_list, &dev->bus->devices);
	spin_unlock(&device_lock);

	if (dev->devt)
		devtmpfs_notify(dev, false);

	device_probe(dev);
	return 0;
}

void device_unregister(struct device *dev)
{
	struct device_driver *drv;

	if (!dev)
		return;

	spin_lock(&device_lock);
	if (!dev->registered) {
		spin_unlock(&device_lock);
		return;
	}
	drv = dev->driver;
	dev->driver = NULL;
	spin_unlock(&device_lock);

	if (drv && drv->remove)
		drv->remove(dev);

	spin_lock(&device_lock);
	if (!list_empty(&dev->bus_list))
		list_del_init(&dev->bus_list);
	if (!list_empty(&dev->device_list))
		list_del_init(&dev->device_list);
	if (dev->parent && !list_empty(&dev->child_node))
		list_del_init(&dev->child_node);
	dev->registered = false;
	spin_unlock(&device_lock);

	if (dev->devt)
		devtmpfs_notify(dev, true);
}

struct device **device_snapshot(size_t *count)
{
	struct device **devices;
	struct device *dev;
	size_t nr = 0;
	size_t i = 0;

	*count = 0;

	spin_lock(&device_lock);
	list_for_each_entry(dev, &device_list, device_list)
		nr++;
	spin_unlock(&device_lock);

	if (!nr)
		return NULL;

	devices = kcalloc(nr, sizeof(*devices));
	if (!devices)
		return NULL;

	spin_lock(&device_lock);
	list_for_each_entry(dev, &device_list, device_list) {
		if (i < nr)
			devices[i++] = dev;
	}
	spin_unlock(&device_lock);

	*count = i;
	return devices;
}

void device_snapshot_free(struct device **devices)
{
	kfree(devices);
}

int driver_register(struct device_driver *drv)
{
	struct device **devices;
	size_t count;
	size_t i;

	if (!drv || !drv->name || !drv->bus)
		return -EINVAL;

	spin_lock(&device_lock);
	list_add_tail(&drv->bus_list, &drv->bus->drivers);
	spin_unlock(&device_lock);

	devices = device_snapshot(&count);
	for (i = 0; i < count; ++i) {
		if (devices[i]->bus == drv->bus)
			device_probe(devices[i]);
	}
	device_snapshot_free(devices);
	return 0;
}

void driver_unregister(struct device_driver *drv)
{
	struct device **devices;
	struct device *dev;
	size_t count;
	size_t i;

	if (!drv)
		return;

	spin_lock(&device_lock);
	if (!list_empty(&drv->bus_list))
		list_del_init(&drv->bus_list);
	spin_unlock(&device_lock);

	devices = device_snapshot(&count);
	for (i = 0; i < count; ++i) {
		dev = devices[i];
		if (dev->driver != drv)
			continue;

		spin_lock(&device_lock);
		dev->driver = NULL;
		spin_unlock(&device_lock);

		if (drv->remove)
			drv->remove(dev);
	}
	device_snapshot_free(devices);
}

/* ---- platform bus ---- */

struct bus_type platform_bus = {
	.name = "platform",
};

static int platform_match(struct device *dev, struct device_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	const struct device_id *id;

	if (!pdev->compatible || !drv->id_table)
		return 0;

	for (id = drv->id_table; id->compatible; ++id) {
		if (!strcmp(id->compatible, pdev->compatible))
			return 1;
	}
	return 0;
}

static void platform_bus_init(void)
{
	platform_bus.match = platform_match;
	bus_register(&platform_bus);
}
core_initcall(platform_bus_init);

int platform_device_register_simple(const char *name, const char *compatible,
				    u64 phys_base, u64 size, u32 irq,
				    void *pdata)
{
	struct platform_device *pdev;
	int err;

	if (!name || !compatible)
		return -EINVAL;

	pdev = kzalloc(sizeof(*pdev));
	if (!pdev)
		return -ENOMEM;

	device_init(&pdev->dev, name);
	pdev->dev.bus = &platform_bus;
	pdev->phys_base = phys_base;
	pdev->size = size;
	pdev->irq = irq;
	pdev->compatible = compatible;
	pdev->pdata = pdata;

	err = device_register(&pdev->dev);
	if (err) {
		kfree(pdev);
		return err;
	}

	return 0;
}

void platform_device_unregister(struct platform_device *pdev)
{
	if (!pdev)
		return;
	device_unregister(&pdev->dev);
	kfree(pdev);
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
