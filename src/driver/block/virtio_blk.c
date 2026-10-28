#include <arch/mm.h>
#include <brk/assert.h>
#include <brk/bio.h>
#include <brk/blkdev.h>
#include <brk/devnode.h>
#include <brk/dt.h>
#include <brk/fs.h>
#include <brk/genhd.h>
#include <brk/initcall.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
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
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>
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
		panic("%s(): invalid used id %" PRIu32 "\n", __func__, id);

	io = vblk->slots[id].trans;
	io->completed = true;
	task_wake_all(&io->completed);
}

static void virtio_blk_irq_handler(void *ctx)
{
	struct virtio_blk_dev *vblk = ctx;
	irqstate_t flags = spin_lock_irqsave(&vblk->lock);

	virtio_mmio_ack_interrupt(vblk->vdev);

	__sync_synchronize();

	virtq_process_used(&vblk->vq, virtio_blk_vq_used, vblk);

	spin_unlock_irqrestore(&vblk->lock, flags);
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

static int virtio_blk_queue_rq(struct request_queue *q, struct request *rq)
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

	pr_info("%s: capacity: %" PRIu64 "\n", __func__,
		vblk->config.capacity);

	snprintf(name, sizeof(name), "virtio_blk%u", slot);
	disk = gendisk_alloc(vblk_registry->major,
			     vblk_registry->minor_start +
				     slot * VIRTIO_BLK_MINORS_PER_DEV,
			     VIRTIO_BLK_MINORS_PER_DEV, name,
			     vblk->config.capacity, vblk);
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

void virtio_blk_init(void)
{
	dev_t dev = 0;
	int err;

	dt_init_scan_virtio_dev();

	vblk_registry = &__vblk_registry;
	spinlock_init(&vblk_registry->lock, "virtio_blk_driver");
	vblk_registry->vblks = kcalloc(VIRTIO_BLK_MAX_DEVS,
				       sizeof(vblk_registry->vblks[0]));
	if (!vblk_registry->vblks)
		panic("%s(): kcalloc failed\n", __func__);
	vblk_registry->disks = kcalloc(VIRTIO_BLK_MAX_DEVS,
				       sizeof(vblk_registry->disks[0]));
	if (!vblk_registry->disks)
		panic("%s(): kcalloc failed\n", __func__);
	vblk_registry->num_vblks = VIRTIO_BLK_MAX_DEVS;

	err = blkdev_alloc_region(VIRTIO_BLK_MAJOR, VIRTIO_BLK_MINOR_START,
				  VIRTIO_BLK_MINOR_COUNT, &dev);
	if (err)
		panic("%s(): blkdev_alloc_region failed\n", __func__);
	vblk_registry->major = MAJOR(dev);
	vblk_registry->minor_start = MINOR(dev);

	pr_info("%s: major: %u\n", __func__, vblk_registry->major);
	pr_info("%s: minor: %u\n", __func__, vblk_registry->minor_start);
}
postcore_initcall(virtio_blk_init);

int virtio_blk_enable_irq(u32 hart_id)
{
	int err = 0;

	if (!vblk_registry)
		return -EINVAL;

	spin_lock(&vblk_registry->lock);
	for (unsigned i = 0; i < vblk_registry->num_vblks; i++) {
		if (vblk_registry->vblks[i]) {
			err = irq_enable_source(
				hart_id, vblk_registry->vblks[i]->vdev->irq);
			if (err)
				break;
		}
	}
	spin_unlock(&vblk_registry->lock);
	return err;
}
