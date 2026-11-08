#define pr_fmt(fmt) "device: " fmt
#include <brk/device.h>
#include <brk/devtmpfs.h>
#include <brk/initcall.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/of.h>
#include <brk/platform.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <uapi/errno.h>

static LIST_DEFINE(bus_list);
static LIST_DEFINE(device_list);
static LIST_DEFINE(deferred_list);
static SPINLOCK_DEFINE(device_lock);

void device_initialize(struct device *dev)
{
	if (!dev)
		return;

	spinlock_init(&dev->lock, "device");
	refcnt_init(&dev->refcnt, 1);
	list_init(&dev->bus_list);
	list_init(&dev->device_list);
	list_init(&dev->children);
	list_init(&dev->child_node);
	list_init(&dev->deferred_node);
	dev->driver = NULL;
	dev->state = DEVICE_STATE_NEW;
	dev->registered = false;
}

void device_init(struct device *dev, const char *name)
{
	if (!dev)
		return;

	memset(dev, 0, sizeof(*dev));
	if (name)
		strlcpy(dev->name, name, sizeof(dev->name));
	device_initialize(dev);
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

struct device *device_get(struct device *dev)
{
	if (!dev)
		return NULL;
	if (!refcnt_inc_unless_zero(&dev->refcnt))
		return NULL;
	return dev;
}

void device_put(struct device *dev)
{
	if (!dev)
		return;

	if (refcnt_dec_fetch(&dev->refcnt) != 0)
		return;

	if (dev->release)
		dev->release(dev);
}

/* Caller must hold device_lock. */
static void device_defer_locked(struct device *dev)
{
	if (list_empty(&dev->deferred_node))
		list_add_tail(&dev->deferred_node, &deferred_list);
}

/* Caller must hold device_lock. */
static void device_undefer_locked(struct device *dev)
{
	if (!list_empty(&dev->deferred_node))
		list_del_init(&dev->deferred_node);
}

static void device_probe(struct device *dev)
{
	struct device_driver *drv = NULL;
	enum device_state state;
	bool claimed = false;
	bool remove = false;
	int err;

	if (!dev || !device_get(dev))
		return;

	if (!dev->bus || !dev->bus->match)
		goto out;

	spin_lock(&device_lock);
	spin_lock(&dev->lock);
	state = dev->state;
	spin_unlock(&dev->lock);

	if (state != DEVICE_STATE_ADDED && state != DEVICE_STATE_DEFERRED) {
		spin_unlock(&device_lock);
		goto out;
	}

	list_for_each_entry(drv, &dev->bus->drivers, bus_list) {
		if (!dev->bus->match(dev, drv))
			continue;

		/* Claim the device before dropping the lock. */
		spin_lock(&dev->lock);
		dev->driver = drv;
		dev->state = DEVICE_STATE_PROBING;
		spin_unlock(&dev->lock);
		claimed = true;
		break;
	}
	spin_unlock(&device_lock);

	if (!claimed)
		goto out;

	err = drv->probe ? drv->probe(dev) : 0;

	spin_lock(&device_lock);
	spin_lock(&dev->lock);
	if (dev->driver == drv) {
		if (dev->state == DEVICE_STATE_REMOVED) {
			/* device_del() raced with probe. */
			dev->driver = NULL;
			device_undefer_locked(dev);
			remove = !err && drv->remove;
		} else if (!drv->registered) {
			/* driver_unregister() raced with probe. */
			dev->driver = NULL;
			dev->state = DEVICE_STATE_ADDED;
			device_undefer_locked(dev);
			remove = !err && drv->remove;
		} else if (!err) {
			dev->state = DEVICE_STATE_BOUND;
			device_undefer_locked(dev);
		} else if (err == -EPROBE_DEFER) {
			dev->driver = NULL;
			dev->state = DEVICE_STATE_DEFERRED;
			device_defer_locked(dev);
		} else {
			dev->driver = NULL;
			dev->state = DEVICE_STATE_ADDED;
			device_undefer_locked(dev);
		}
	}
	spin_unlock(&dev->lock);
	spin_unlock(&device_lock);

	if (remove)
		drv->remove(dev);
	if (err && err != -EPROBE_DEFER)
		pr_warn("device: %s: probe by %s failed: %d\n", dev->name,
			drv->name, err);
out:
	device_put(dev);
}

int device_add(struct device *dev)
{
	enum device_state state;

	if (!dev || !dev->name[0])
		return -EINVAL;

	spin_lock(&device_lock);
	spin_lock(&dev->lock);
	state = dev->state;
	if (dev->registered || state != DEVICE_STATE_NEW) {
		spin_unlock(&dev->lock);
		spin_unlock(&device_lock);
		return -EBUSY;
	}

	dev->registered = true;
	dev->state = DEVICE_STATE_ADDED;
	spin_unlock(&dev->lock);

	list_add_tail(&dev->device_list, &device_list);
	if (dev->parent)
		list_add_tail(&dev->child_node, &dev->parent->children);
	if (dev->bus)
		list_add_tail(&dev->bus_list, &dev->bus->devices);
	spin_unlock(&device_lock);

	pr_info("register %s\n", dev->name);

	if (dev->devt)
		devtmpfs_notify(dev, false);

	device_probe(dev);
	return 0;
}

int device_register(struct device *dev)
{
	return device_add(dev);
}

void device_del(struct device *dev)
{
	struct device_driver *drv;
	enum device_state state;
	bool call_remove = false;

	if (!dev)
		return;

	spin_lock(&device_lock);
	spin_lock(&dev->lock);
	if (!dev->registered) {
		spin_unlock(&dev->lock);
		spin_unlock(&device_lock);
		return;
	}

	dev->registered = false;
	state = dev->state;
	drv = dev->driver;
	dev->state = DEVICE_STATE_REMOVED;

	if (state != DEVICE_STATE_PROBING) {
		dev->driver = NULL;
		call_remove = state == DEVICE_STATE_BOUND && drv && drv->remove;
	}
	/* A running probe unbinds itself in device_probe(). */
	spin_unlock(&dev->lock);

	if (!list_empty(&dev->bus_list))
		list_del_init(&dev->bus_list);
	if (!list_empty(&dev->device_list))
		list_del_init(&dev->device_list);
	if (dev->parent && !list_empty(&dev->child_node))
		list_del_init(&dev->child_node);
	device_undefer_locked(dev);
	spin_unlock(&device_lock);

	if (call_remove)
		drv->remove(dev);

	if (dev->devt)
		devtmpfs_notify(dev, true);
}

void device_unregister(struct device *dev)
{
	if (!dev)
		return;

	device_del(dev);
	device_put(dev);
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
		if (i < nr && device_get(dev))
			devices[i++] = dev;
	}
	spin_unlock(&device_lock);

	*count = i;
	return devices;
}

void device_snapshot_free(struct device **devices, size_t count)
{
	size_t i;

	if (!devices)
		return;

	for (i = 0; i < count; ++i)
		device_put(devices[i]);
	kfree(devices);
}

static void device_probe_deferred(struct bus_type *bus)
{
	struct device **devices;
	struct device *dev;
	size_t nr = 0;
	size_t i = 0;
	size_t j;

	spin_lock(&device_lock);
	list_for_each_entry(dev, &deferred_list, deferred_node) {
		if (!bus || dev->bus == bus)
			nr++;
	}
	spin_unlock(&device_lock);

	if (!nr)
		return;

	devices = kcalloc(nr, sizeof(*devices));
	if (!devices)
		return;

	spin_lock(&device_lock);
	list_for_each_entry(dev, &deferred_list, deferred_node) {
		if (bus && dev->bus != bus)
			continue;
		if (i < nr && device_get(dev))
			devices[i++] = dev;
	}
	spin_unlock(&device_lock);

	for (j = 0; j < i; ++j) {
		device_probe(devices[j]);
		device_put(devices[j]);
	}
	kfree(devices);
}

int driver_register(struct device_driver *drv)
{
	struct device_driver *other;
	struct device **devices;
	size_t count;
	size_t i;

	if (!drv || !drv->name || !drv->bus)
		return -EINVAL;

	spin_lock(&device_lock);
	if (drv->registered) {
		spin_unlock(&device_lock);
		return -EBUSY;
	}

	list_for_each_entry(other, &drv->bus->drivers, bus_list) {
		if (!strcmp(other->name, drv->name)) {
			spin_unlock(&device_lock);
			return -EEXIST;
		}
	}

	list_init(&drv->bus_list);
	drv->registered = true;
	list_add_tail(&drv->bus_list, &drv->bus->drivers);
	spin_unlock(&device_lock);

	devices = device_snapshot(&count);
	for (i = 0; i < count; ++i) {
		if (devices[i]->bus == drv->bus)
			device_probe(devices[i]);
	}
	device_snapshot_free(devices, count);

	/* Retry devices that deferred waiting for this driver. */
	device_probe_deferred(drv->bus);
	return 0;
}

void driver_unregister(struct device_driver *drv)
{
	struct device **devices;
	size_t count;
	size_t i;

	if (!drv || !drv->registered)
		return;

	spin_lock(&device_lock);
	if (!list_empty(&drv->bus_list))
		list_del_init(&drv->bus_list);
	drv->registered = false;
	spin_unlock(&device_lock);

	devices = device_snapshot(&count);
	for (i = 0; i < count; ++i) {
		struct device *dev = devices[i];
		enum device_state state;
		bool call_remove = false;

		spin_lock(&device_lock);
		spin_lock(&dev->lock);
		if (dev->driver == drv) {
			state = dev->state;
			if (state != DEVICE_STATE_PROBING &&
			    state != DEVICE_STATE_REMOVED) {
				dev->driver = NULL;
				dev->state = DEVICE_STATE_ADDED;
				device_undefer_locked(dev);
				call_remove = state == DEVICE_STATE_BOUND &&
					      drv->remove;
			}
			/* A running probe unbinds itself in device_probe(). */
		}
		spin_unlock(&dev->lock);
		spin_unlock(&device_lock);

		if (call_remove)
			drv->remove(dev);
	}
	device_snapshot_free(devices, count);
}

static void device_deferred_probe_init(void)
{
	device_probe_deferred(NULL);
}
late_initcall(device_deferred_probe_init);

/* ---- platform bus ---- */

struct bus_type platform_bus = {
	.name = "platform",
};

static int platform_match(struct device *dev, struct device_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	const struct device_id *id;

	if (!drv->id_table)
		return 0;

	if (dev->of_node) {
		for (id = drv->id_table; id->compatible; ++id) {
			if (of_device_is_compatible(dev->of_node,
						    id->compatible))
				return 1;
		}
	}

	if (!pdev->compatible)
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
