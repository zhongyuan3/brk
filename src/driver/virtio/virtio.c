#include <arch/pgtable.h>
#include <brk/initcall.h>
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
	return dev;
}

void virtio_dev_destroy(struct virtio_device *dev)
{
	virtio_dev_remove(dev);
	iounmap(dev->mem_base, dev->size);
	kfree(dev);
}

void virtio_dev_add(struct virtio_device *dev)
{
	spin_lock(&vdevs_lock);
	list_add(&dev->list, &vdevs);
	spin_unlock(&vdevs_lock);
}

void virtio_dev_remove(struct virtio_device *dev)
{
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

void virtio_init_scan(void)
{
	struct virtio_device **devs;
	struct virtio_device *dev;
	struct virtio_blk_dev *vblk;
	size_t count = 0;
	size_t i = 0;

	spin_lock(&vdevs_lock);
	list_for_each_entry(dev, &vdevs, list)
		count++;
	spin_unlock(&vdevs_lock);

	if (count == 0)
		return;

	devs = kcalloc(count, sizeof(*devs));
	if (!devs)
		return;

	spin_lock(&vdevs_lock);
	list_for_each_entry(dev, &vdevs, list) {
		if (i < count)
			devs[i++] = dev;
	}
	spin_unlock(&vdevs_lock);

	/*
	 * Driver creation/registration may allocate memory, ioremap and
	 * register IRQs; do it without vdevs_lock held.
	 */
	for (size_t j = 0; j < i; ++j) {
		dev = devs[j];
		if (dev->id != VIRTIO_DEVICE_ID_BLK)
			continue;

		vblk = virtio_blk_create(dev,
					 VIRTIO_BLK_DEFAULT_QUEUE_SIZE);
		if (!vblk)
			continue;
		if (virtio_blk_register(vblk))
			virtio_blk_destroy(vblk);
	}

	kfree(devs);
}
subsys_initcall(virtio_init_scan);
