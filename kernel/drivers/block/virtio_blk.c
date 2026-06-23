#include <arch/mm.h>
#include <brk/assert.h>
#include <brk/blkdev.h>
#include <brk/fs.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_mmio.h>
#include <brk/virtio_queue.h>
#include <uapi/brk/errno.h>
#include <uapi/fcntl.h>
#include <uapi/types.h>

static struct virtio_blk_registry __vblk_registry;
static struct virtio_blk_registry *vblk_registry;

static int virtio_blk_status_to_errno(char status)
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

static int virtio_blk_validate_dev(struct virtio_device *vdev)
{
	if (vdev->id != VIRTIO_DEVICE_ID_BLK)
		return -EINVAL;
	return 0;
}

static u32 virtio_blk_select_features(struct virtio_device *vdev)
{
	u32 features = virtio_mmio_read_features(vdev);

	features &= ~(1u << VIRTIO_BLK_F_RO);
	features &= ~(1u << VIRTIO_BLK_F_SCSI);
	features &= ~(1u << VIRTIO_BLK_F_CONFIG_WCE);
	features &= ~(1u << VIRTIO_BLK_F_MQ);
	features &= ~(1u << VIRTIO_F_ANY_LAYOUT);
	features &= ~(1u << VIRTIO_F_EVENT_IDX);
	features &= ~(1u << VIRTIO_F_INDIRECT_DESC);
	return features;
}

static int virtio_blk_alloc_vq(struct virtio_blk_dev *vblk,
			       unsigned int queue_size)
{
	int err;

	err = virtq_alloc(&vblk->vq, queue_size);
	if (err)
		return err;

	vblk->reqs = kcalloc(queue_size, sizeof(struct virtio_blk_req));
	if (!vblk->reqs)
		goto err_reqs;

	vblk->slots = kcalloc(queue_size, sizeof(struct virtio_blk_slot));
	if (!vblk->slots)
		goto err_tracks;

	return 0;

err_tracks:
	kfree(vblk->reqs);
	vblk->reqs = NULL;
err_reqs:
	virtq_free(&vblk->vq);
	return -ENOMEM;
}

static void virtio_blk_free_vq(struct virtio_blk_dev *vblk)
{
	kfree(vblk->slots);
	vblk->slots = NULL;
	kfree(vblk->reqs);
	vblk->reqs = NULL;
	virtq_free(&vblk->vq);
}

static void virtio_blk_vq_used(struct virtq *vq, u32 id, void *ctx)
{
	struct virtio_blk_dev *vblk = ctx;
	struct virtio_blk_io_desc *io;

	(void)vq;

	if (id >= vblk->vq.num || !vblk->slots[id].trans)
		panic("%s(): invalid used id %u\n", __func__, id);

	io = vblk->slots[id].trans;
	io->completed = true;
	task_wake_all(&io->completed);
}

static void virtio_blk_irq_handler(void *ctx)
{
	struct virtio_blk_dev *vblk = ctx;

	spinlock_acquire(&vblk->lock);

	virtio_mmio_ack_interrupt(vblk->vdev);

	__sync_synchronize();

	virtq_process_used(&vblk->vq, virtio_blk_vq_used, vblk);

	spinlock_release(&vblk->lock);
}

static int virtio_blk_submit(struct virtio_blk_dev *vblk,
			     struct virtio_blk_io_desc *io)
{
	unsigned int idx[3];
	struct virtio_blk_req *req;
	char *status;
	int err;

	spinlock_acquire(&vblk->lock);

	while (virtq_alloc_desc_chain(&vblk->vq, idx, 3))
		task_sleep(&vblk->vq.desc, &vblk->lock);

	req = &vblk->reqs[idx[0]];
	req->type = io->is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
	req->reserved = 0;
	req->sector = io->sector;

	vblk->vq.desc[idx[0]].addr = virt_to_phys((u64)req);
	vblk->vq.desc[idx[0]].len = sizeof(*req);
	vblk->vq.desc[idx[0]].flags = VIRTQ_DESC_F_NEXT;
	vblk->vq.desc[idx[0]].next = idx[1];

	vblk->vq.desc[idx[1]].addr = io->buf_phys;
	vblk->vq.desc[idx[1]].len = io->sec_count * VIRTIO_BLK_SECTOR_SIZE;
	vblk->vq.desc[idx[1]].flags = io->is_write ? 0 : VIRTQ_DESC_F_WRITE;
	vblk->vq.desc[idx[1]].flags |= VIRTQ_DESC_F_NEXT;
	vblk->vq.desc[idx[1]].next = idx[2];

	status = &vblk->slots[idx[0]].status;
	*status = 0xff;
	vblk->vq.desc[idx[2]].addr = virt_to_phys((u64)status);
	vblk->vq.desc[idx[2]].len = sizeof(*status);
	vblk->vq.desc[idx[2]].flags = VIRTQ_DESC_F_WRITE;
	vblk->vq.desc[idx[2]].next = 0;

	io->completed = false;
	vblk->slots[idx[0]].trans = io;

	virtq_submit(&vblk->vq, idx[0]);
	virtio_mmio_queue_notify(vblk->vdev, 0);

	while (!io->completed)
		task_sleep(&io->completed, &vblk->lock);

	err = virtio_blk_status_to_errno(vblk->slots[idx[0]].status);
	vblk->slots[idx[0]].trans = NULL;
	virtq_free_desc_chain(&vblk->vq, idx[0]);
	task_wake_all(&vblk->vq.desc);

	spinlock_release(&vblk->lock);
	return err;
}

static int virtio_blk_io(struct virtio_blk_dev *vblk, u64 sector, u64 buf_phys,
			 size_t sec_count, bool write)
{
	struct virtio_blk_io_desc trans = {
		.buf_phys = buf_phys,
		.sector = sector,
		.sec_count = sec_count,
		.is_write = write,
		.completed = false,
	};

	return virtio_blk_submit(vblk, &trans);
}

static int virtio_blk_bdev_rw(struct block_dev *bdev, u64 blk_id, void *buf,
			      u32 blk_cnt, bool write)
{
	int err;
	u64 buf_phys;
	struct virtio_blk_dev *vbd = bdev->priv;

	err = blkdev_check_bounds(bdev, blk_id, blk_cnt);
	if (err)
		return err;

	if (!vbd)
		return -EINVAL;

	buf_phys = virt_to_phys((u64)buf);
	return virtio_blk_io(vbd, blk_id, buf_phys, blk_cnt, write);
}

static int virtio_blk_bdev_read(struct block_dev *bdev, u64 blk_id, void *buf,
				u32 blk_cnt)
{
	return virtio_blk_bdev_rw(bdev, blk_id, buf, blk_cnt, false);
}

static int virtio_blk_bdev_write(struct block_dev *bdev, u64 blk_id,
				 const void *buf, u32 blk_cnt)
{
	return virtio_blk_bdev_rw(bdev, blk_id, (void *)buf, blk_cnt, true);
}

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

	if (!is_power_of_two(vblk->queue_size))
		return -EINVAL;

	err = virtio_blk_validate_dev(vdev);
	if (err)
		return err;

	virtio_mmio_reset(vdev);
	err = virtio_mmio_start_driver(vdev);
	if (err)
		return err;

	features = virtio_blk_select_features(vdev);
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

	err = irq_register_handler(vdev->irq, virtio_blk_irq_handler, vblk,
				   NULL, NULL);
	if (err)
		return err;
	irq_set_priority(vdev->irq, 1);

	return 0;
}

static void virtio_blk_detach(struct virtio_blk_dev *vblk)
{
	irq_unregister_handler(vblk->vdev->irq, NULL, NULL);
}

struct virtio_blk_dev *virtio_blk_create(struct virtio_device *vdev,
					 unsigned queue_size)
{
	struct virtio_blk_dev *vblk;
	int err;

	if (!is_power_of_two(queue_size))
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
	struct block_dev *bdev;
	int err;

	if (!vblk_registry)
		return -EINVAL;

	if (!vblk || !vblk->vdev)
		return -EINVAL;

	bdev = blkdev_alloc();
	if (!bdev)
		return -ENOMEM;

	klog_info("%s: capacity: %lu\n", __func__, vblk->config.capacity);

	bdev->ops.read = virtio_blk_bdev_read;
	bdev->ops.write = virtio_blk_bdev_write;
	bdev->phy_bcnt = vblk->config.capacity;
	bdev->phy_bsize = VIRTIO_BLK_SECTOR_SIZE;
	bdev->priv = vblk;

	spinlock_acquire(&vblk_registry->lock);
	for (unsigned i = 0; i < vblk_registry->num_vblks; i++) {
		if (vblk_registry->vblks[i] == NULL) {
			bdev->dev = MKBLKDEV(vblk_registry->major,
					     vblk_registry->minor_start + i);
			err = blkdev_register(bdev);
			if (err) {
				spinlock_release(&vblk_registry->lock);
				blkdev_free(bdev);
				return err;
			}
			vblk_registry->vblks[i] = vblk;
			vblk_registry->bdevs[i] = bdev;
			spinlock_release(&vblk_registry->lock);
			return 0;
		}
	}
	spinlock_release(&vblk_registry->lock);
	return -EBUSY;
}

void virtio_blk_unregister(struct virtio_blk_dev *vblk)
{
	struct block_dev *bdev = NULL;

	if (!vblk || !vblk->vdev)
		return;

	spinlock_acquire(&vblk_registry->lock);
	for (unsigned i = 0; i < vblk_registry->num_vblks; i++) {
		if (vblk_registry->vblks[i] == vblk) {
			vblk_registry->vblks[i] = NULL;
			bdev = vblk_registry->bdevs[i];
			vblk_registry->bdevs[i] = NULL;
			spinlock_release(&vblk_registry->lock);
			blkdev_unregister(bdev);
			blkdev_free(bdev);
			return;
		}
	}
	spinlock_release(&vblk_registry->lock);
}

int virtio_blk_init(void)
{
	dev_t dev = 0;
	int err;

	vblk_registry = &__vblk_registry;
	spinlock_init(&vblk_registry->lock, "virtio_blk_driver");
	vblk_registry->vblks = kcalloc(VIRTIO_BLK_MINOR_COUNT,
				       sizeof(vblk_registry->vblks[0]));
	if (!vblk_registry->vblks)
		return -ENOMEM;
	vblk_registry->bdevs = kcalloc(VIRTIO_BLK_MINOR_COUNT,
				       sizeof(vblk_registry->bdevs[0]));
	if (!vblk_registry->bdevs)
		return -ENOMEM;
	vblk_registry->num_vblks = VIRTIO_BLK_MINOR_COUNT;

	err = blkdev_alloc_region(VIRTIO_BLK_MAJOR, VIRTIO_BLK_MINOR_START,
				  VIRTIO_BLK_MINOR_COUNT, &dev);
	if (err)
		return err;
	vblk_registry->major = MAJOR(dev);
	vblk_registry->minor_start = MINOR(dev);

	klog_info("%s: major: %u\n", __func__, vblk_registry->major);
	klog_info("%s: minor: %u\n", __func__, vblk_registry->minor_start);

	return 0;
}

int virtio_blk_enable_irq(u32 hart_id)
{
	int err = 0;

	if (!vblk_registry)
		return -EINVAL;

	spinlock_acquire(&vblk_registry->lock);
	for (unsigned i = 0; i < vblk_registry->num_vblks; i++) {
		if (vblk_registry->vblks[i]) {
			err = irq_enable_source(
				hart_id, vblk_registry->vblks[i]->vdev->irq);
			if (err)
				break;
		}
	}
	spinlock_release(&vblk_registry->lock);
	return err;
}

int virtio_blk_mknod(void)
{
	int err = 0;
	char name[32];

	if (!vblk_registry)
		return -EINVAL;

	spinlock_acquire(&vblk_registry->lock);
	for (unsigned i = 0; i < vblk_registry->num_vblks; i++) {
		if (vblk_registry->vblks[i]) {
			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name) - 1, "/dev/virtio_blk%u",
				 i);
			err = do_mknodat(
				AT_FDCWD, name, S_IFBLK,
				MKBLKDEV(vblk_registry->major,
					 vblk_registry->minor_start + i));
			if (err)
				break;
			klog_info("/dev/virtio_blk%u created successfully\n",
				  i);
		}
	}
	spinlock_release(&vblk_registry->lock);

	return err;
}
