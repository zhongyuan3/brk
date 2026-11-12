#include <arch/mm.h>
#include <brk/assert.h>
#include <brk/bio.h>
#include <brk/blkdev.h>
#include <brk/container_of.h>
#include <brk/device.h>
#include <brk/fs.h>
#include <brk/genhd.h>
#include <brk/irq.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/log2.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_mmio.h>
#include <brk/virtio_queue.h>
#include <inttypes.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>
#include <uapi/types.h>

#include "virtio_blk_internal.h"

static struct virtio_blk_registry __vblk_registry;
static struct virtio_blk_registry *vblk_registry;

static struct virtio_blk_dev *virtio_blk_alloc(unsigned queue_size)
{
	struct virtio_blk_dev *vblk;
	int err;

	vblk = kzalloc(sizeof(*vblk));
	if (!vblk)
		return NULL;

	spinlock_init(&vblk->lock, "virtio_blk_dev");
	vblk->queue_size = queue_size;
	err = virtio_blk_alloc_vq(vblk, queue_size);
	if (err) {
		kfree(vblk);
		return NULL;
	}
	return vblk;
}

static void virtio_blk_free(struct virtio_blk_dev *vblk)
{
	if (!vblk)
		return;
	virtio_blk_free_vq(vblk);
	kfree(vblk);
}

static int virtio_blk_setup(struct virtio_blk_dev *vblk,
			    struct virtio_device *vdev)
{
	u32 features;
	int err;

	if (!vblk || !vdev)
		return -EINVAL;

	if (!is_power_of_2(vblk->queue_size))
		return -EINVAL;

	err = virtio_blk_validate_dev(vdev);
	if (err)
		return err;

	virtio_mmio_reset(vdev);
	err = virtio_mmio_start_driver(vdev);
	if (err)
		return err;

	features = virtio_blk_select_features(vdev);
	vblk->flush_supported = (features & (1u << VIRTIO_BLK_F_FLUSH)) != 0;
	virtio_mmio_write_features(vdev, features);

	err = virtio_mmio_features_ok(vdev);
	if (err)
		return err;

	err = virtio_mmio_setup_queue(vdev, 0, &vblk->vq, vblk->queue_size);
	if (err)
		return err;

	err = virtio_mmio_driver_ok(vdev);
	if (err)
		return err;

	vblk->vdev = vdev;

	err = virtio_mmio_read(vdev, &vblk->config, sizeof(vblk->config),
			       VIRTIO_CONFIG_OFFSET);
	if (err)
		return err;

	err = request_irq(vdev->irq, virtio_blk_irq_handler, 0, "virtio_blk",
			  vblk);
	if (err)
		return err;

	return 0;
}

static void virtio_blk_detach(struct virtio_blk_dev *vblk)
{
	free_irq(vblk->vdev->irq, vblk);
}

struct virtio_blk_dev *virtio_blk_create(struct virtio_device *vdev,
					 unsigned queue_size)
{
	struct virtio_blk_dev *vblk;
	int err;

	if (!is_power_of_2(queue_size))
		return NULL;

	vblk = virtio_blk_alloc(queue_size);
	if (!vblk)
		return NULL;

	err = virtio_blk_setup(vblk, vdev);
	if (err) {
		virtio_blk_free(vblk);
		return NULL;
	}

	return vblk;
}

void virtio_blk_destroy(struct virtio_blk_dev *vblk)
{
	if (!vblk)
		return;
	virtio_blk_detach(vblk);
	virtio_blk_free(vblk);
}

int virtio_blk_register(struct virtio_blk_dev *vblk)
{
	struct gendisk *disk;
	char name[32];
	unsigned int slot;
	int err;

	if (!vblk_registry)
		return -EINVAL;
	if (!vblk || !vblk->vdev)
		return -EINVAL;

	spin_lock(&vblk_registry->lock);
	for (slot = 0; slot < vblk_registry->num_vblks; ++slot) {
		if (!vblk_registry->vblks[slot])
			break;
	}
	if (slot == vblk_registry->num_vblks) {
		spin_unlock(&vblk_registry->lock);
		return -EBUSY;
	}
	vblk_registry->vblks[slot] = vblk;
	spin_unlock(&vblk_registry->lock);

	snprintf(name, sizeof(name), "virtio_blk%u", slot);
	disk = gendisk_alloc(vblk_registry->major,
			     vblk_registry->minor_start +
				     slot * VIRTIO_BLK_MINORS_PER_DEV,
			     VIRTIO_BLK_MINORS_PER_DEV, name,
			     vblk->config.capacity, &vblk->vdev->dev, vblk);
	if (!disk) {
		err = -ENOMEM;
		goto clear_slot;
	}

	err = blk_init_queue(disk, virtio_blk_queue_rq);
	if (err) {
		gendisk_free(disk);
		goto clear_slot;
	}

	err = add_disk(disk);
	if (err) {
		gendisk_free(disk);
		goto clear_slot;
	}

	spin_lock(&vblk_registry->lock);
	vblk_registry->disks[slot] = disk;
	spin_unlock(&vblk_registry->lock);
	return 0;

clear_slot:
	spin_lock(&vblk_registry->lock);
	vblk_registry->vblks[slot] = NULL;
	spin_unlock(&vblk_registry->lock);
	return err;
}

void virtio_blk_unregister(struct virtio_blk_dev *vblk)
{
	struct gendisk *disk = NULL;

	if (!vblk || !vblk->vdev)
		return;

	spin_lock(&vblk_registry->lock);
	for (unsigned i = 0; i < vblk_registry->num_vblks; i++) {
		if (vblk_registry->vblks[i] == vblk) {
			vblk_registry->vblks[i] = NULL;
			disk = vblk_registry->disks[i];
			vblk_registry->disks[i] = NULL;
			break;
		}
	}
	spin_unlock(&vblk_registry->lock);

	del_gendisk(disk);
}

static int virtio_blk_probe(struct device *dev)
{
	struct virtio_device *vdev =
		container_of(dev, struct virtio_device, dev);
	struct virtio_blk_dev *vblk;
	int err;

	vblk = virtio_blk_create(vdev, VIRTIO_BLK_DEFAULT_QUEUE_SIZE);
	if (!vblk)
		return -ENOMEM;

	err = virtio_blk_register(vblk);
	if (err) {
		virtio_blk_destroy(vblk);
		return err;
	}

	dev->driver_data = vblk;
	return 0;
}

static void virtio_blk_remove(struct device *dev)
{
	struct virtio_blk_dev *vblk = dev->driver_data;

	if (!vblk)
		return;

	dev->driver_data = NULL;
	virtio_blk_unregister(vblk);
	virtio_blk_destroy(vblk);
}

static const struct device_id virtio_blk_ids[] = {
	{ .device_id = VIRTIO_DEVICE_ID_BLK },
	{},
};

static struct device_driver virtio_blk_driver = {
	.name = "virtio_blk",
	.bus = &virtio_bus,
	.id_table = virtio_blk_ids,
	.probe = virtio_blk_probe,
	.remove = virtio_blk_remove,
};

void virtio_blk_init(void)
{
	dev_t dev = 0;
	int err;

	vblk_registry = &__vblk_registry;
	spinlock_init(&vblk_registry->lock, "virtio_blk_driver");
	vblk_registry->vblks =
		kcalloc(VIRTIO_BLK_MAX_DEVS, sizeof(vblk_registry->vblks[0]));
	if (!vblk_registry->vblks)
		panic("%s(): kcalloc failed\n", __func__);
	vblk_registry->disks =
		kcalloc(VIRTIO_BLK_MAX_DEVS, sizeof(vblk_registry->disks[0]));
	if (!vblk_registry->disks)
		panic("%s(): kcalloc failed\n", __func__);
	vblk_registry->num_vblks = VIRTIO_BLK_MAX_DEVS;

	err = blkdev_alloc_region(VIRTIO_BLK_MAJOR, VIRTIO_BLK_MINOR_START,
				  VIRTIO_BLK_MINOR_COUNT, &dev);
	if (err)
		panic("%s(): blkdev_alloc_region failed\n", __func__);
	vblk_registry->major = MAJOR(dev);
	vblk_registry->minor_start = MINOR(dev);

	err = driver_register(&virtio_blk_driver);
	if (err)
		panic("%s(): driver_register failed\n", __func__);
}
