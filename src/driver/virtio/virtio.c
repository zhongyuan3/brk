#include <arch/pgtable.h>
#include <brk/device_model.h>
#include <brk/initcall.h>
#include <brk/kernel.h>
#include <brk/printf.h>
#include <brk/ioremap.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mmio.h>
#include <brk/spinlock.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_mmio.h>
#include <uapi/errno.h>

static LIST_DEFINE(vdevs);
static SPINLOCK_DEFINE(vdevs_lock);
static unsigned int virtio_dev_index;

struct bus_type virtio_bus = {
	.name = "virtio",
};

static int virtio_match(struct device *dev, struct device_driver *drv)
{
	struct virtio_device *vdev = container_of(dev, struct virtio_device,
						  dev);
	const struct device_id *id;

	if (!drv->id_table)
		return 0;

	for (id = drv->id_table; id->device_id; ++id) {
		if (id->device_id == vdev->id)
			return 1;
	}
	return 0;
}

static void virtio_bus_init(void)
{
	virtio_bus.match = virtio_match;
	bus_register(&virtio_bus);
}
core_initcall(virtio_bus_init);

struct virtio_device *virtio_dev_create(u64 phys_base, size_t size, u32 irq)
{
	struct virtio_device *dev;
	int err;

	dev = kmalloc(sizeof(*dev));
	if (!dev)
		return NULL;

	dev->phys_base = phys_base;
	dev->size = size;
	dev->irq = irq;

	dev->mem_base = ioremap(phys_base, size, PTE_R | PTE_W);
	if (!dev->mem_base) {
		kfree(dev);
		return NULL;
	}

	list_init(&dev->list);

	err = virtio_mmio_probe(dev);
	if (err) {
		iounmap(dev->mem_base, dev->size);
		kfree(dev);
		return NULL;
	}

	dev->id = readl(dev->mem_base + VIRTIO_DEVICE_ID_OFFSET);

	char name[DEVICE_NAME_LEN];

	snprintf(name, sizeof(name), "virtio%u", virtio_dev_index++);
	device_init(&dev->dev, name);
	dev->dev.bus = &virtio_bus;
	return dev;
}

void virtio_dev_destroy(struct virtio_device *dev)
{
	virtio_dev_remove(dev);
	iounmap(dev->mem_base, dev->size);
	kfree(dev);
}

int virtio_dev_add(struct virtio_device *dev)
{
	spin_lock(&vdevs_lock);
	list_add(&dev->list, &vdevs);
	spin_unlock(&vdevs_lock);

	/* On failure the caller owns cleanup via virtio_dev_destroy(). */
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
