#include <aosd/ioremap.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/mmio.h>
#include <aosd/pgtable.h>
#include <aosd/slab.h>
#include <aosd/spinlock.h>
#include <aosd/virtio.h>

static list_head_define(vdevs);
static spinlock_define(vdevs_lock);

struct virtio_device *virtio_dev_create(uint64_t phys_base, size_t size,
					uint32_t irq)
{
	struct virtio_device *dev;

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

	list_init_head(&dev->list);

	dev->id = readl(dev->mem_base + VIRTIO_DEVICE_ID_OFFSET);

	return dev;
}

void virtio_dev_destroy(struct virtio_device *dev)
{
	iounmap(dev->mem_base, dev->size);
	kfree(dev);
}

void virtio_dev_add(struct virtio_device *dev)
{
	spinlock_acquire(&vdevs_lock);
	list_add(&dev->list, &vdevs);
	spinlock_release(&vdevs_lock);
}

void virtio_dev_remove(struct virtio_device *dev)
{
	spinlock_acquire(&vdevs_lock);
	list_del(&dev->list);
	spinlock_release(&vdevs_lock);
}

struct virtio_device *virtio_dev_get(uint32_t id)
{
	struct virtio_device *dev;

	spinlock_acquire(&vdevs_lock);
	list_for_each_entry(dev, &vdevs, list) {
		if (dev->id == id) {
			spinlock_release(&vdevs_lock);
			return dev;
		}
	}
	spinlock_release(&vdevs_lock);

	return NULL;
}
