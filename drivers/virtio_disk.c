#include <brk/assert.h>
#include <brk/blkdev.h>
#include <brk/fs.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_disk.h>
#include <brk/virtio_mmio.h>
#include <brk/virtio_queue.h>
#include <uapi/brk/errno.h>
#include <uapi/fcntl.h>
#include <uapi/types.h>

static struct virtio_disk_driver __vdisk_driver;
static struct virtio_disk_driver *vdisk_driver;

static int virtio_blk_status_errno(char status)
{
	switch (status) {
	case VIRTIO_BLK_S_OK:
		return 0;
	case VIRTIO_BLK_S_IOERR:
		return -EIO;
	case VIRTIO_BLK_S_UNSUPP:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

static int virtio_blk_init_check(struct virtio_device *dev)
{
	if (dev->id != VIRTIO_DEVICE_ID_BLK)
		return -EINVAL;
	return 0;
}

static u32 virtio_blk_negotiate_features(struct virtio_device *dev)
{
	u32 features = virtio_mmio_read_features(dev);

	features &= ~(1u << VIRTIO_BLK_F_RO);
	features &= ~(1u << VIRTIO_BLK_F_SCSI);
	features &= ~(1u << VIRTIO_BLK_F_CONFIG_WCE);
	features &= ~(1u << VIRTIO_BLK_F_MQ);
	features &= ~(1u << VIRTIO_F_ANY_LAYOUT);
	features &= ~(1u << VIRTIO_F_EVENT_IDX);
	features &= ~(1u << VIRTIO_F_INDIRECT_DESC);
	return features;
}

static int virtio_disk_init_alloc(struct virtio_disk_device *bdev,
				  unsigned int queue_size)
{
	int err;

	err = virtq_alloc(&bdev->vq, queue_size);
	if (err)
		return err;

	bdev->reqs = kcalloc(queue_size, sizeof(struct virtio_blk_req));
	if (!bdev->reqs)
		goto err_reqs;

	bdev->tracks = kcalloc(queue_size, sizeof(struct virtio_disk_track));
	if (!bdev->tracks)
		goto err_tracks;

	return 0;

err_tracks:
	kfree(bdev->reqs);
	bdev->reqs = NULL;
err_reqs:
	virtq_free(&bdev->vq);
	return -ENOMEM;
}

static void virtio_disk_finalize_free(struct virtio_disk_device *bdev)
{
	kfree(bdev->tracks);
	bdev->tracks = NULL;
	kfree(bdev->reqs);
	bdev->reqs = NULL;
	virtq_free(&bdev->vq);
}

static void virtio_blk_used_cb(struct virtq *vq, u32 id, void *ctx)
{
	struct virtio_disk_device *bdev = ctx;
	struct virtio_disk_transaction *trans;

	(void)vq;

	if (id >= bdev->vq.num || !bdev->tracks[id].trans)
		panic("%s(): invalid used id %u\n", __func__, id);

	trans = bdev->tracks[id].trans;
	trans->completed = true;
	proc_wake_up(&trans->completed);
}

static void virtio_disk_intr(void *ctx)
{
	struct virtio_disk_device *bdev = ctx;

	spinlock_acquire(&bdev->lock);

	virtio_mmio_ack_interrupt(bdev->vdev);

	__sync_synchronize();

	virtq_process_used(&bdev->vq, virtio_blk_used_cb, bdev);

	spinlock_release(&bdev->lock);
}

static int virtio_blk_transfer(struct virtio_disk_device *bdev,
			       struct virtio_disk_transaction *trans)
{
	unsigned int idx[3];
	struct virtio_blk_req *req;
	char *status;
	int err;

	spinlock_acquire(&bdev->lock);

	while (virtq_alloc_desc_chain(&bdev->vq, idx, 3))
		proc_sleep(&bdev->vq.desc, &bdev->lock);

	req = &bdev->reqs[idx[0]];
	req->type = trans->is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
	req->reserved = 0;
	req->sector = trans->sector;

	bdev->vq.desc[idx[0]].addr = virt_to_phys((u64)req);
	bdev->vq.desc[idx[0]].len = sizeof(*req);
	bdev->vq.desc[idx[0]].flags = VIRTQ_DESC_F_NEXT;
	bdev->vq.desc[idx[0]].next = idx[1];

	bdev->vq.desc[idx[1]].addr = trans->buf_phys;
	bdev->vq.desc[idx[1]].len = trans->sec_count * SECTOR_SIZE;
	bdev->vq.desc[idx[1]].flags = trans->is_write ? 0 : VIRTQ_DESC_F_WRITE;
	bdev->vq.desc[idx[1]].flags |= VIRTQ_DESC_F_NEXT;
	bdev->vq.desc[idx[1]].next = idx[2];

	status = &bdev->tracks[idx[0]].status;
	*status = 0xff;
	bdev->vq.desc[idx[2]].addr = virt_to_phys((u64)status);
	bdev->vq.desc[idx[2]].len = sizeof(*status);
	bdev->vq.desc[idx[2]].flags = VIRTQ_DESC_F_WRITE;
	bdev->vq.desc[idx[2]].next = 0;

	trans->completed = false;
	bdev->tracks[idx[0]].trans = trans;

	virtq_submit(&bdev->vq, idx[0]);
	virtio_mmio_queue_notify(bdev->vdev, 0);

	while (!trans->completed)
		proc_sleep(&trans->completed, &bdev->lock);

	err = virtio_blk_status_errno(bdev->tracks[idx[0]].status);
	bdev->tracks[idx[0]].trans = NULL;
	virtq_free_desc_chain(&bdev->vq, idx[0]);
	proc_wake_up(&bdev->vq.desc);

	spinlock_release(&bdev->lock);
	return err;
}

static int virtio_blk_rw(struct virtio_disk_device *bdev, u64 sector,
			 u64 buf_phys, usize_t sec_count, bool write)
{
	struct virtio_disk_transaction trans = {
		.buf_phys = buf_phys,
		.sector = sector,
		.sec_count = sec_count,
		.is_write = write,
		.completed = false,
	};

	return virtio_blk_transfer(bdev, &trans);
}

static int virtio_blk_read(struct virtio_disk_device *bdev, u64 sector,
			   u64 buf_phys, usize_t sec_count)
{
	return virtio_blk_rw(bdev, sector, buf_phys, sec_count, false);
}

static int virtio_blk_write(struct virtio_disk_device *bdev, u64 sector,
			    u64 buf_phys, usize_t sec_count)
{
	return virtio_blk_rw(bdev, sector, buf_phys, sec_count, true);
}

static int virtio_disk_rw(struct block_dev *bd, u64 blk_id, void *buf,
			  u32 blk_cnt, bool write)
{
	int err;
	u64 buf_phys;
	struct virtio_disk_device *disk = bd->priv;

	err = blkdev_check_bounds(bd, blk_id, blk_cnt);
	if (err)
		return err;

	if (!disk)
		return -EINVAL;

	buf_phys = virt_to_phys((u64)buf);
	if (write)
		return virtio_blk_write(disk, blk_id, buf_phys, blk_cnt);
	return virtio_blk_read(disk, blk_id, buf_phys, blk_cnt);
}

static int virtio_disk_read(struct block_dev *bd, u64 blk_id, void *buf,
			    u32 blk_cnt)
{
	return virtio_disk_rw(bd, blk_id, buf, blk_cnt, false);
}

static int virtio_disk_write(struct block_dev *bd, u64 blk_id, const void *buf,
			     u32 blk_cnt)
{
	return virtio_disk_rw(bd, blk_id, (void *)buf, blk_cnt, true);
}

static struct virtio_disk_device *virtio_disk_device_alloc(unsigned queue_size)
{
	struct virtio_disk_device *disk;
	int err;

	disk = kzalloc(sizeof(*disk));
	if (!disk)
		return NULL;

	spinlock_init(&disk->lock, "virtio_disk");
	disk->queue_size = queue_size;
	err = virtio_disk_init_alloc(disk, queue_size);
	if (err) {
		kfree(disk);
		return NULL;
	}
	return disk;
}

static void virtio_disk_device_free(struct virtio_disk_device *bdev)
{
	if (!bdev)
		return;
	virtio_disk_finalize_free(bdev);
	kfree(bdev);
}

static int virtio_disk_device_init(struct virtio_disk_device *disk,
				   struct virtio_device *dev)
{
	u32 features;
	int err;

	if (!disk || !dev)
		return -EINVAL;

	if (!is_power_of_two(disk->queue_size))
		return -EINVAL;

	err = virtio_blk_init_check(dev);
	if (err)
		return err;

	virtio_mmio_reset(dev);
	err = virtio_mmio_start_driver(dev);
	if (err)
		return err;

	features = virtio_blk_negotiate_features(dev);
	virtio_mmio_write_features(dev, features);

	err = virtio_mmio_features_ok(dev);
	if (err)
		return err;

	err = virtio_mmio_setup_queue(dev, 0, &disk->vq, disk->queue_size);
	if (err)
		return err;

	err = virtio_mmio_driver_ok(dev);
	if (err)
		return err;

	disk->vdev = dev;

	err = virtio_mmio_read(dev, &disk->config, sizeof(disk->config),
			       VIRTIO_CONFIG_OFFSET);
	if (err)
		return err;

	err = irq_register_handler(dev->irq, virtio_disk_intr, disk, NULL,
				   NULL);
	if (err)
		return err;
	irq_set_priority(dev->irq, 1);

	return 0;
}

static void virtio_disk_device_finalize(struct virtio_disk_device *disk)
{
	irq_unregister_handler(disk->vdev->irq, NULL, NULL);
}

struct virtio_disk_device *virtio_disk_device_create(struct virtio_device *dev,
						     unsigned queue_size)
{
	struct virtio_disk_device *disk;
	int err;

	if (!is_power_of_two(queue_size))
		return NULL;

	disk = virtio_disk_device_alloc(queue_size);
	if (!disk)
		return NULL;

	err = virtio_disk_device_init(disk, dev);
	if (err) {
		virtio_disk_device_free(disk);
		return NULL;
	}

	return disk;
}

void virtio_disk_device_destroy(struct virtio_disk_device *disk)
{
	if (!disk)
		return;
	virtio_disk_device_finalize(disk);
	virtio_disk_device_free(disk);
}

int virtio_disk_add_device(struct virtio_disk_device *disk)
{
	struct block_dev *bd;
	int err;

	if (!vdisk_driver)
		return -EINVAL;

	if (!disk || !disk->vdev)
		return -EINVAL;

	bd = blkdev_alloc();
	if (!bd)
		return -ENOMEM;

	klog_info("%s: capacity: %lu\n", __func__, disk->config.capacity);

	bd->ops.read = virtio_disk_read;
	bd->ops.write = virtio_disk_write;
	bd->phy_bcnt = disk->config.capacity;
	bd->phy_bsize = SECTOR_SIZE;
	bd->priv = disk;

	spinlock_acquire(&vdisk_driver->lock);
	for (unsigned i = 0; i < vdisk_driver->num_disks; i++) {
		if (vdisk_driver->disks[i] == NULL) {
			bd->dev = MKBLKDEV(vdisk_driver->major,
					   vdisk_driver->minor_start + i);
			err = blkdev_register(bd);
			if (err) {
				spinlock_release(&vdisk_driver->lock);
				blkdev_free(bd);
				return err;
			}
			vdisk_driver->disks[i] = disk;
			vdisk_driver->bdevs[i] = bd;
			spinlock_release(&vdisk_driver->lock);
			return 0;
		}
	}
	spinlock_release(&vdisk_driver->lock);
	return -EBUSY;
}

void virtio_disk_remove_device(struct virtio_disk_device *disk)
{
	struct block_dev *bdev = NULL;

	if (!disk || !disk->vdev)
		return;

	spinlock_acquire(&vdisk_driver->lock);
	for (unsigned i = 0; i < vdisk_driver->num_disks; i++) {
		if (vdisk_driver->disks[i] == disk) {
			vdisk_driver->disks[i] = NULL;
			bdev = vdisk_driver->bdevs[i];
			vdisk_driver->bdevs[i] = NULL;
			spinlock_release(&vdisk_driver->lock);
			blkdev_unregister(bdev);
			blkdev_free(bdev);
			return;
		}
	}
	spinlock_release(&vdisk_driver->lock);
}

int virtio_disk_driver_init(void)
{
	dev_t dev = 0;
	int err;

	vdisk_driver = &__vdisk_driver;
	spinlock_init(&vdisk_driver->lock, "vdisk_driver");
	vdisk_driver->disks = kcalloc(VIRTIO_DISK_MINOR_COUNT,
				      sizeof(vdisk_driver->disks[0]));
	if (!vdisk_driver->disks)
		return -ENOMEM;
	vdisk_driver->bdevs = kcalloc(VIRTIO_DISK_MINOR_COUNT,
				      sizeof(vdisk_driver->bdevs[0]));
	if (!vdisk_driver->bdevs)
		return -ENOMEM;
	vdisk_driver->num_disks = VIRTIO_DISK_MINOR_COUNT;

	err = blkdev_alloc_region(VIRTIO_DISK_MAJOR, VIRTIO_DISK_MINOR_START,
				  VIRTIO_DISK_MINOR_COUNT, &dev);
	if (err)
		return err;
	vdisk_driver->major = MAJOR(dev);
	vdisk_driver->minor_start = MINOR(dev);

	klog_info("%s: major: %u\n", __func__, vdisk_driver->major);
	klog_info("%s: minor: %u\n", __func__, vdisk_driver->minor_start);

	return 0;
}

int virtio_disk_enable_irq(u32 hart_id)
{
	int err = 0;

	if (!vdisk_driver)
		return -EINVAL;

	spinlock_acquire(&vdisk_driver->lock);
	for (unsigned i = 0; i < vdisk_driver->num_disks; i++) {
		if (vdisk_driver->disks[i]) {
			err = irq_enable_source(
				hart_id, vdisk_driver->disks[i]->vdev->irq);
			if (err)
				break;
		}
	}
	spinlock_release(&vdisk_driver->lock);
	return err;
}

int virtio_disk_create_fs_nodes(void)
{
	int err = 0;
	char name[32];

	if (!vdisk_driver)
		return -EINVAL;

	spinlock_acquire(&vdisk_driver->lock);
	for (unsigned i = 0; i < vdisk_driver->num_disks; i++) {
		if (vdisk_driver->disks[i]) {
			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name) - 1, "/dev/vdisk%u", i);
			err = do_mknodat(
				AT_FDCWD, name, S_IFBLK,
				MKBLKDEV(vdisk_driver->major,
					 vdisk_driver->minor_start + i));
			if (err)
				break;
			klog_info("/dev/vdisk%u created successfully\n", i);
		}
	}
	spinlock_release(&vdisk_driver->lock);

	return err;
}
