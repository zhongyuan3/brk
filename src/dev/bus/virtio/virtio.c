#include <brk/container_of.h>
#include <brk/device.h>
#include <brk/list.h>
#include <brk/spinlock.h>
#include <brk/virtio.h>
#include <stdbool.h>

static LIST_DEFINE(vdevs);
static SPINLOCK_DEFINE(vdevs_lock);

struct bus_type virtio_bus = {
	.name = "virtio",
};

static inline struct virtio_device *to_virtio_device(struct device *dev)
{
	return container_of(dev, struct virtio_device, dev);
}

static bool virtio_match(struct device *dev, struct device_driver *drv)
{
	struct virtio_device *vdev = to_virtio_device(dev);
	const struct device_id *id;

	if (!drv->id_table)
		return false;

	for (id = drv->id_table; id->device_id; ++id) {
		if (id->device_id == vdev->id)
			return true;
	}
	return false;
}

void virtio_bus_init(void)
{
	virtio_bus.match = virtio_match;
	bus_register(&virtio_bus);
}

int virtio_dev_add(struct virtio_device *dev)
{
	spin_lock(&vdevs_lock);
	list_add(&dev->list, &vdevs);
	spin_unlock(&vdevs_lock);

	/* On failure the caller owns cleanup via virtio_mmio_dev_destroy(). */
	return device_register(&dev->dev);
}

void virtio_dev_remove(struct virtio_device *dev)
{
	device_unregister(&dev->dev);

	spin_lock(&vdevs_lock);
	list_del(&dev->list);
	spin_unlock(&vdevs_lock);
}

struct virtio_device *virtio_dev_get(u32 id)
{
	struct virtio_device *dev;

	spin_lock(&vdevs_lock);
	list_for_each_entry(dev, &vdevs, list) {
		if (dev->id == id) {
			spin_unlock(&vdevs_lock);
			return dev;
		}
	}
	spin_unlock(&vdevs_lock);

	return NULL;
}
