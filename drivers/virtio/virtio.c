#include <aosd/ioremap.h>
#include <aosd/list.h>
#include <aosd/mmio.h>
#include <aosd/pgtable.h>
#include <aosd/slab.h>
#include <aosd/virtio.h>

static LIST_HEAD(virtio_dev_list);

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
	list_add(&dev->list, &virtio_dev_list);
}

void virtio_dev_remove(struct virtio_device *dev)
{
	list_del(&dev->list);
}

struct virtio_device *virtio_dev_get(uint32_t id)
{
	struct virtio_device *dev;
	list_for_each_entry(dev, &virtio_dev_list, list)
		if (dev->id == id)
			return dev;

	return NULL;
}
