#include <brk/errno.h>
#include <brk/ioremap.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mmio.h>
#include <brk/pgtable.h>
#include <brk/slab.h>
#include <brk/virtio.h>
#include <brk/virtio_disk.h>
#include <brk/virtio_mmio.h>

static LIST_DEFINE(vdevs);
static SPINLOCK_DEFINE(vdevs_lock);

struct virtio_device *virtio_dev_create(u64 phys_base, usize_t size, u32 irq)
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

struct virtio_device *virtio_dev_get(u32 id)
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

void virtio_init_scan(void)
{
	struct virtio_device *dev;
	struct virtio_disk_device *disk;
	int err = 0;

	spinlock_acquire(&vdevs_lock);
	list_for_each_entry(dev, &vdevs, list) {
		if (dev->id == VIRTIO_DEVICE_ID_BLK) {
			disk = virtio_disk_device_create(
				dev, VIRTIO_BLK_DEFAULT_QUEUE_SIZE);
			if (!disk)
				continue;
			err = virtio_disk_add_device(disk);
			if (err) {
				virtio_disk_device_destroy(disk);
				continue;
			}
		}
	}
	spinlock_release(&vdevs_lock);
}
