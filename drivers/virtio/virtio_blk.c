#include <brk/assert.h>
#include <brk/errno.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_mmio.h>
#include <brk/virtio_queue.h>

struct virtio_blk_dev {
	struct virtio_device *vdev;
	struct virtq vq;
	struct virtio_blk_req *reqs;
	struct virtio_blk_track *tracks;
	spinlock_t lock;
};

static struct virtio_blk_dev blk;

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

static int virtio_blk_init_alloc(struct virtio_blk_dev *bdev,
				 unsigned int queue_size)
{
	int err;

	err = virtq_alloc(&bdev->vq, queue_size);
	if (err)
		return err;

	bdev->reqs = kcalloc(queue_size, sizeof(struct virtio_blk_req));
	if (!bdev->reqs)
		goto err_reqs;

	bdev->tracks = kcalloc(queue_size, sizeof(struct virtio_blk_track));
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

static void virtio_blk_used_cb(struct virtq *vq, u32 id, void *ctx)
{
	struct virtio_blk_dev *bdev = ctx;
	struct virtio_blk_transaction *trans;

	(void)vq;

	if (id >= bdev->vq.num || !bdev->tracks[id].trans)
		panic("%s(): invalid used id %u\n", __func__, id);

	trans = bdev->tracks[id].trans;
	trans->completed = true;
	proc_wake_up(&trans->completed);
}

int virtio_blk_init(struct virtio_device *dev, unsigned int queue_size)
{
	struct virtio_blk_dev *bdev = &blk;
	u32 features;
	int err;

	if (!dev)
		return -EINVAL;

	if (!is_power_of_two(queue_size))
		return -EINVAL;

	err = virtio_blk_init_check(dev);
	if (err)
		return err;

	err = virtio_blk_init_alloc(bdev, queue_size);
	if (err)
		return err;

	virtio_mmio_reset(dev);
	err = virtio_mmio_start_driver(dev);
	if (err)
		goto err_deinit;

	features = virtio_blk_negotiate_features(dev);
	virtio_mmio_write_features(dev, features);

	err = virtio_mmio_features_ok(dev);
	if (err)
		goto err_deinit;

	err = virtio_mmio_setup_queue(dev, 0, &bdev->vq, queue_size);
	if (err)
		goto err_deinit;

	err = virtio_mmio_driver_ok(dev);
	if (err)
		goto err_deinit;

	bdev->vdev = dev;
	spinlock_init(&bdev->lock, "virtio_blk");

	irq_register_handler(dev->irq, virtio_blk_intr, NULL);
	plic_set_priority(dev->irq, 1);

	return 0;

err_deinit:
	virtq_free(&bdev->vq);
	kfree(bdev->tracks);
	bdev->tracks = NULL;
	kfree(bdev->reqs);
	bdev->reqs = NULL;
	return err;
}

void virtio_blk_init_hart(u32 hart_id)
{
	if (!blk.vdev)
		return;
	plic_enable(hart_id, blk.vdev->irq);
}

static int virtio_blk_transfer(struct virtio_blk_transaction *trans)
{
	struct virtio_blk_dev *bdev = &blk;
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

static int virtio_blk_rw(u64 sector, u64 buf_phys, usize_t sec_count,
			 bool write)
{
	struct virtio_blk_transaction trans = {
		.buf_phys = buf_phys,
		.sector = sector,
		.sec_count = sec_count,
		.is_write = write,
		.completed = false,
	};

	return virtio_blk_transfer(&trans);
}

int virtio_blk_read(u64 sector, u64 buf_phys, usize_t sec_count)
{
	return virtio_blk_rw(sector, buf_phys, sec_count, false);
}

int virtio_blk_write(u64 sector, u64 buf_phys, usize_t sec_count)
{
	return virtio_blk_rw(sector, buf_phys, sec_count, true);
}

void virtio_blk_intr(void)
{
	struct virtio_blk_dev *bdev = &blk;

	spinlock_acquire(&bdev->lock);

	virtio_mmio_ack_interrupt(bdev->vdev);

	__sync_synchronize();

	virtq_process_used(&bdev->vq, virtio_blk_used_cb, bdev);

	spinlock_release(&bdev->lock);
}
