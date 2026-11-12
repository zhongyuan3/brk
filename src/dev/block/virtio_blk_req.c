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

int virtio_blk_validate_dev(struct virtio_device *vdev)
{
	if (vdev->id != VIRTIO_DEVICE_ID_BLK)
		return -EINVAL;
	return 0;
}

u32 virtio_blk_select_features(struct virtio_device *vdev)
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

int virtio_blk_alloc_vq(struct virtio_blk_dev *vblk, unsigned int queue_size)
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

void virtio_blk_free_vq(struct virtio_blk_dev *vblk)
{
	kfree(vblk->slots);
	vblk->slots = NULL;
	kfree(vblk->reqs);
	vblk->reqs = NULL;
	virtq_free(&vblk->vq);
}

static void virtio_blk_vq_used(struct virtq *vq, u32 id, u32 len, void *ctx)
{
	struct virtio_blk_dev *vblk = ctx;
	struct virtio_blk_io_desc *io;

	(void)vq;
	(void)len;

	if (id >= vblk->vq.num || !vblk->slots[id].trans)
		panic("%s(): invalid used id %" PRIu32 "\n", __func__, id);

	io = vblk->slots[id].trans;
	io->completed = true;
	task_wake_all(&io->completed);
}

irqreturn_t virtio_blk_irq_handler(int irq, void *dev_id)
{
	struct virtio_blk_dev *vblk = dev_id;
	irqstate_t flags = spin_lock_irqsave(&vblk->lock);

	(void)irq;

	virtio_mmio_ack_interrupt(vblk->vdev);

	__sync_synchronize();

	virtq_process_used(&vblk->vq, virtio_blk_vq_used, vblk);

	spin_unlock_irqrestore(&vblk->lock, flags);
	return IRQ_HANDLED;
}

#define VIRTIO_BLK_MAX_SEGS 16

static int virtio_blk_submit_vec(struct virtio_blk_dev *vblk, u64 sector,
				 const struct bio_vec *vecs,
				 unsigned int nr_vecs, bool write)
{
	unsigned int idx[VIRTIO_BLK_MAX_SEGS + 2];
	unsigned int chain = nr_vecs + 2;
	unsigned int sidx;
	struct virtio_blk_req *req;
	struct virtio_blk_io_desc trans = { .completed = false };
	char *status;
	int err;
	irqstate_t flags = spin_lock_irqsave(&vblk->lock);

	if (!nr_vecs || nr_vecs > VIRTIO_BLK_MAX_SEGS) {
		spin_unlock_irqrestore(&vblk->lock, flags);
		return -EINVAL;
	}

	while (virtq_alloc_desc_chain(&vblk->vq, idx, chain))
		task_sleep(&vblk->vq.desc, &vblk->lock);

	req = &vblk->reqs[idx[0]];
	req->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
	req->reserved = 0;
	req->sector = sector;

	vblk->vq.desc[idx[0]].addr = virt_to_phys((uintptr_t)req);
	vblk->vq.desc[idx[0]].len = sizeof(*req);
	vblk->vq.desc[idx[0]].flags = VIRTQ_DESC_F_NEXT;
	vblk->vq.desc[idx[0]].next = idx[1];

	for (unsigned int i = 0; i < nr_vecs; ++i) {
		struct virtq_desc *desc = &vblk->vq.desc[idx[i + 1]];

		desc->addr = virt_to_phys((uintptr_t)vecs[i].bv_buf);
		desc->len = vecs[i].bv_len;
		desc->flags = write ? 0 : VIRTQ_DESC_F_WRITE;
		desc->flags |= VIRTQ_DESC_F_NEXT;
		desc->next = idx[i + 2];
	}

	status = &vblk->slots[idx[0]].status;
	*status = 0xff;
	sidx = idx[nr_vecs + 1];
	vblk->vq.desc[sidx].addr = virt_to_phys((uintptr_t)status);
	vblk->vq.desc[sidx].len = sizeof(*status);
	vblk->vq.desc[sidx].flags = VIRTQ_DESC_F_WRITE;
	vblk->vq.desc[sidx].next = 0;

	vblk->slots[idx[0]].trans = &trans;
	virtq_submit(&vblk->vq, idx[0]);
	virtio_mmio_queue_notify(vblk->vdev, 0);

	while (!trans.completed)
		task_sleep(&trans.completed, &vblk->lock);

	err = virtio_blk_status_to_errno(vblk->slots[idx[0]].status);
	vblk->slots[idx[0]].trans = NULL;
	virtq_free_desc_chain(&vblk->vq, idx[0]);
	task_wake_all(&vblk->vq.desc);

	spin_unlock_irqrestore(&vblk->lock, flags);
	return err;
}

static int virtio_blk_flush(struct virtio_blk_dev *vblk)
{
	unsigned int idx[2];
	struct virtio_blk_req *req;
	struct virtio_blk_io_desc trans = { .completed = false };
	char *status;
	int err;
	irqstate_t flags = spin_lock_irqsave(&vblk->lock);

	while (virtq_alloc_desc_chain(&vblk->vq, idx, 2))
		task_sleep(&vblk->vq.desc, &vblk->lock);

	req = &vblk->reqs[idx[0]];
	req->type = VIRTIO_BLK_T_FLUSH;
	req->reserved = 0;
	req->sector = 0;

	vblk->vq.desc[idx[0]].addr = virt_to_phys((uintptr_t)req);
	vblk->vq.desc[idx[0]].len = sizeof(*req);
	vblk->vq.desc[idx[0]].flags = VIRTQ_DESC_F_NEXT;
	vblk->vq.desc[idx[0]].next = idx[1];

	status = &vblk->slots[idx[0]].status;
	*status = 0xff;
	vblk->vq.desc[idx[1]].addr = virt_to_phys((uintptr_t)status);
	vblk->vq.desc[idx[1]].len = sizeof(*status);
	vblk->vq.desc[idx[1]].flags = VIRTQ_DESC_F_WRITE;
	vblk->vq.desc[idx[1]].next = 0;

	vblk->slots[idx[0]].trans = &trans;
	virtq_submit(&vblk->vq, idx[0]);
	virtio_mmio_queue_notify(vblk->vdev, 0);

	while (!trans.completed)
		task_sleep(&trans.completed, &vblk->lock);

	err = virtio_blk_status_to_errno(vblk->slots[idx[0]].status);
	vblk->slots[idx[0]].trans = NULL;
	virtq_free_desc_chain(&vblk->vq, idx[0]);
	task_wake_all(&vblk->vq.desc);

	spin_unlock_irqrestore(&vblk->lock, flags);
	return err;
}

int virtio_blk_queue_rq(struct request_queue *q, struct request *rq)
{
	struct gendisk *disk = q->disk;
	struct virtio_blk_dev *vblk = disk->priv;
	struct bio_vec segs[VIRTIO_BLK_MAX_SEGS];
	unsigned int nr = 0;
	u64 sector = rq->rq_start;
	u64 seg_sector = rq->rq_start;
	bool write = rq->rq_op == REQ_OP_WRITE;
	int err;

	if (!vblk)
		return -ENODEV;

	if (rq->rq_op == REQ_OP_FLUSH)
		return vblk->flush_supported ? virtio_blk_flush(vblk) : 0;

	for (struct bio *bio = rq->rq_bio; bio; bio = bio->bi_next) {
		for (unsigned int i = 0; i < bio->bi_nr_vecs; ++i) {
			if (!nr)
				seg_sector = sector;
			segs[nr++] = bio->bi_io_vec[i];
			sector += bio->bi_io_vec[i].bv_len >> 9;

			if (nr == VIRTIO_BLK_MAX_SEGS) {
				err = virtio_blk_submit_vec(vblk, seg_sector,
							    segs, nr, write);
				if (err)
					return err;
				nr = 0;
			}
		}
	}

	if (nr) {
		err = virtio_blk_submit_vec(vblk, seg_sector, segs, nr, write);
		if (err)
			return err;
	}

	return 0;
}
