#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/errno.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/panic.h>
#include <brk/plic.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_queue.h>

static struct virtio_device *blk_dev;
static struct virtq blk_vq;
static struct virtio_blk_req *blk_reqs;
static struct virtio_blk_track *blk_tracks;
static bool *blk_desc_used;
static struct kmem_cache blk_trans_cache;
static uint16_t blk_used_idx;
static SPINLOCK_DEFINE(blk_lock);

static int virtio_blk_init_alloc(uint32_t queue_size)
{
	int err = -ENOMEM;
	size_t size;

	blk_vq.desc = kcalloc(queue_size, sizeof(struct virtq_desc));
	if (!blk_vq.desc)
		goto err0;

	size = sizeof(struct virtq_avail) + sizeof(uint16_t) * queue_size;
	blk_vq.avail = kzalloc(size);
	if (!blk_vq.avail)
		goto err1;

	size = sizeof(struct virtq_used) +
	       sizeof(struct virtq_used_elem) * queue_size;
	blk_vq.used = kzalloc(size);
	if (!blk_vq.used)
		goto err2;

	blk_reqs = kcalloc(queue_size, sizeof(struct virtio_blk_req));
	if (!blk_reqs)
		goto err3;

	blk_tracks = kcalloc(queue_size, sizeof(struct virtio_blk_track));
	if (!blk_tracks)
		goto err4;

	blk_desc_used = kcalloc(queue_size, sizeof(bool));
	if (!blk_desc_used)
		goto err5;

	err = kmem_cache_init(&blk_trans_cache,
			      sizeof(struct virtio_blk_transation),
			      alignof(struct virtio_blk_transation),
			      "blk_trans_cache");
	if (err)
		goto err6;

	blk_vq.num = queue_size;
	return 0;

err6:
	kfree(blk_desc_used);
	blk_desc_used = NULL;
err5:
	kfree(blk_tracks);
	blk_tracks = NULL;
err4:
	kfree(blk_reqs);
	blk_reqs = NULL;
err3:
	kfree(blk_vq.used);
	blk_vq.used = NULL;
err2:
	kfree(blk_vq.avail);
	blk_vq.avail = NULL;
err1:
	kfree(blk_vq.desc);
	blk_vq.desc = NULL;
err0:
	return err;
}

static int virtio_blk_init_check(struct virtio_device *dev)
{
	uint64_t mem_base = (uint64_t)dev->mem_base;

	if (dev->id != VIRTIO_DEVICE_ID_BLK)
		return -EINVAL;

	if (readl(mem_base + VIRTIO_MAGIC_VALUE_OFFSET) != VIRTIO_MAGIC_VALUE)
		return -EINVAL;

	if (readl(mem_base + VIRTIO_VERSION_OFFSET) != 2)
		return -EINVAL;

	if (readl(mem_base + VIRTIO_DEVICE_ID_OFFSET) != VIRTIO_DEVICE_ID_BLK)
		return -EINVAL;

	if (readl(mem_base + VIRTIO_VENDOR_ID_OFFSET) != VIRTIO_VENDOR_ID)
		return -EINVAL;

	return 0;
}

int virtio_blk_init(struct virtio_device *dev, unsigned int queue_size)
{
	uint64_t mem_base;
	uint32_t status;
	uint64_t features;
	int err;
	uint64_t paddr;
	uint32_t queue_size_max;

	err = virtio_blk_init_check(dev);
	if (err)
		return err;

	err = virtio_blk_init_alloc(queue_size);
	if (err)
		return err;

	mem_base = (uint64_t)dev->mem_base;

	/* reset device */
	status = 0;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	/* set ACKNOWLEDGE status bit */
	status |= VIRTIO_STATUS_ACKNOWLEDGE;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	/* set DRIVER status bit */
	status |= VIRTIO_STATUS_DRIVER;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	/* negotiate features */
	features = readl(mem_base + VIRTIO_DEVICE_FEATURES_OFFSET);
	features &= ~(1 << VIRTIO_BLK_F_RO);
	features &= ~(1 << VIRTIO_BLK_F_SCSI);
	features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
	features &= ~(1 << VIRTIO_BLK_F_MQ);
	features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
	features &= ~(1 << VIRTIO_F_EVENT_IDX);
	features &= ~(1 << VIRTIO_F_INDIRECT_DESC);
	writel(features, mem_base + VIRTIO_DRIVER_FEATURES_OFFSET);

	/* tell device that feature negotiation is complete. */
	status |= VIRTIO_STATUS_FEATURES_OK;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	/* re-read status to ensure FEATURES_OK is set. */
	status = readl(mem_base + VIRTIO_STATUS_OFFSET);
	if (!(status & VIRTIO_STATUS_FEATURES_OK))
		panic("%s(): VIRTIO_STATUS_FEATURES_OK unset\n", __func__);

	/* initialize queue 0. */
	writel(0, mem_base + VIRTIO_QUEUE_SEL_OFFSET);

	/* ensure queue 0 is not in use. */
	if (readl(mem_base + VIRTIO_QUEUE_READY_OFFSET))
		panic("%s(): queue should not be ready\n", __func__);

	/* check maximum queue size. */
	queue_size_max = readl(mem_base + VIRTIO_QUEUE_SIZE_MAX_OFFSET);
	if (queue_size_max == 0)
		panic("%s(): virtio blk has no queue 0\n", __func__);
	if (queue_size_max < queue_size)
		panic("%s(): virtio blk max queue too short\n", __func__);

	/* set queue size. */
	writel(queue_size, mem_base + VIRTIO_QUEUE_SIZE_OFFSET);

	/* write physical addresses. */
	paddr = virt_to_phys((uint64_t)blk_vq.desc);
	writel(paddr & 0xffffffff, mem_base + VIRTIO_QUEUE_DESC_LOW_OFFSET);
	writel(paddr >> 32, mem_base + VIRTIO_QUEUE_DESC_HIGH_OFFSET);
	paddr = virt_to_phys((uint64_t)blk_vq.avail);
	writel(paddr & 0xffffffff, mem_base + VIRTIO_QUEUE_DRIVER_LOW_OFFSET);
	writel(paddr >> 32, mem_base + VIRTIO_QUEUE_DRIVER_HIGH_OFFSET);
	paddr = virt_to_phys((uint64_t)blk_vq.used);
	writel(paddr & 0xffffffff, mem_base + VIRTIO_QUEUE_DEVICE_LOW_OFFSET);
	writel(paddr >> 32, mem_base + VIRTIO_QUEUE_DEVICE_HIGH_OFFSET);

	/* queue is ready. */
	writel(1, mem_base + VIRTIO_QUEUE_READY_OFFSET);

	/* tell device we're completely ready. */
	status |= VIRTIO_STATUS_DRIVER_OK;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	blk_dev = dev;

	irq_register_handler(blk_dev->irq, virtio_blk_intr, NULL);
	plic_set_priority(blk_dev->irq, 1);

	return 0;
}

void virtio_blk_init_hart(uint32_t hart_id)
{
	plic_enable(hart_id, blk_dev->irq);
}

static int alloc_desc(unsigned int *desc_idx)
{
	for (unsigned int i = 0; i < blk_vq.num; i++) {
		if (!blk_desc_used[i]) {
			blk_desc_used[i] = true;
			*desc_idx = i;
			return 0;
		}
	}
	return -ENOMEM;
}

static void free_desc(unsigned int desc_idx)
{
	ASSERT(desc_idx < blk_vq.num);
	ASSERT(blk_desc_used[desc_idx]);
	blk_desc_used[desc_idx] = false;
	blk_vq.desc[desc_idx].addr = 0;
	blk_vq.desc[desc_idx].len = 0;
	blk_vq.desc[desc_idx].flags = 0;
	blk_vq.desc[desc_idx].next = 0;
}

static void free_desc_chain(unsigned int desc_idx)
{
	while (1) {
		unsigned int flags = blk_vq.desc[desc_idx].flags;
		unsigned int next = blk_vq.desc[desc_idx].next;
		free_desc(desc_idx);
		if (flags & VIRTQ_DESC_F_NEXT)
			desc_idx = next;
		else
			break;
	}
}

static int alloc_desc_chain(unsigned int *desc_idx, unsigned int num)
{
	for (unsigned int i = 0; i < num; i++) {
		if (alloc_desc(desc_idx + i)) {
			for (unsigned int j = 0; j < i; j++)
				free_desc(desc_idx[j]);
			return -ENOMEM;
		}
	}
	return 0;
}

static int virtio_blk_transfer(struct virtio_blk_transation *trans)
{
	unsigned int idx[3];
	struct virtio_blk_req *req;
	char *status;

	spinlock_acquire(&blk_lock);

	while (1) {
		if (alloc_desc_chain(idx, 3) == 0)
			break;
		proc_sleep(&blk_vq.desc, &blk_lock);
	}

	req = &blk_reqs[idx[0]];
	if (trans->is_write)
		req->type = VIRTIO_BLK_T_OUT;
	else
		req->type = VIRTIO_BLK_T_IN;
	req->reserved = 0;
	req->sector = trans->sector;

	blk_vq.desc[idx[0]].addr = virt_to_phys((uint64_t)req);
	blk_vq.desc[idx[0]].len = sizeof(struct virtio_blk_req);
	blk_vq.desc[idx[0]].flags = VIRTQ_DESC_F_NEXT;
	blk_vq.desc[idx[0]].next = idx[1];

	blk_vq.desc[idx[1]].addr = trans->buf_phys;
	blk_vq.desc[idx[1]].len = trans->sec_count * SECTOR_SIZE;
	if (trans->is_write)
		blk_vq.desc[idx[1]].flags = 0;
	else
		blk_vq.desc[idx[1]].flags = VIRTQ_DESC_F_WRITE;
	blk_vq.desc[idx[1]].flags |= VIRTQ_DESC_F_NEXT;
	blk_vq.desc[idx[1]].next = idx[2];

	status = &blk_tracks[idx[0]].status;
	*status = 0xff;
	blk_vq.desc[idx[2]].addr = virt_to_phys((uint64_t)status);
	blk_vq.desc[idx[2]].len = sizeof(*status);
	blk_vq.desc[idx[2]].flags = VIRTQ_DESC_F_WRITE;
	blk_vq.desc[idx[2]].next = 0;

	trans->completed = false;
	blk_tracks[idx[0]].trans = trans;

	blk_vq.avail->ring[blk_vq.avail->idx % blk_vq.num] = idx[0];

	__sync_synchronize();

	blk_vq.avail->idx += 1;

	__sync_synchronize();

	writel(0, blk_dev->mem_base + VIRTIO_QUEUE_NOTIFY_OFFSET);

	while (!trans->completed)
		proc_sleep(&trans->completed, &blk_lock);

	blk_tracks[idx[0]].trans = NULL;
	free_desc_chain(idx[0]);
	proc_wake_up(&blk_vq.desc);

	spinlock_release(&blk_lock);

	return 0;
}

int virtio_blk_read(uint64_t sector, uint64_t buf, size_t sec_count)
{
	struct virtio_blk_transation *trans;
	int err;

	trans = kmem_cache_alloc(&blk_trans_cache);
	if (!trans)
		return -ENOMEM;
	trans->buf_phys = buf;
	trans->sector = sector;
	trans->sec_count = sec_count;
	trans->is_write = false;
	trans->completed = false;
	err = virtio_blk_transfer(trans);
	kmem_cache_free(&blk_trans_cache, trans);
	if (err)
		return err;
	return 0;
}

int virtio_blk_write(uint64_t sector, uint64_t buf, size_t sec_count)
{
	struct virtio_blk_transation *trans;
	int err;

	trans = kmem_cache_alloc(&blk_trans_cache);
	if (!trans)
		return -ENOMEM;
	trans->buf_phys = buf;
	trans->sector = sector;
	trans->sec_count = sec_count;
	trans->is_write = true;
	trans->completed = false;
	err = virtio_blk_transfer(trans);
	kmem_cache_free(&blk_trans_cache, trans);
	if (err)
		return err;
	return 0;
}

void virtio_blk_intr(void)
{
	uint32_t status;
	uint32_t id;
	struct virtio_blk_transation *trans;
	uint16_t curr_used_idx;
	uint64_t mem_base;

	spinlock_acquire(&blk_lock);

	mem_base = (uint64_t)blk_dev->mem_base;

	status = readl(mem_base + VIRTIO_INTERRUPT_STATUS_OFFSET) & 0x3;
	writel(status, mem_base + VIRTIO_INTERRUPT_ACK_OFFSET);

	__sync_synchronize();

	curr_used_idx = blk_vq.used->idx;
	while (blk_used_idx != curr_used_idx) {
		__sync_synchronize();
		id = blk_vq.used->ring[blk_used_idx % blk_vq.num].id;

		if (id >= blk_vq.num || blk_tracks[id].status != 0) {
			spinlock_release(&blk_lock);
			panic("%s(): invalid track status %d\n", __func__,
			      blk_tracks[id].status);
		}

		trans = blk_tracks[id].trans;
		trans->completed = true;
		proc_wake_up(&trans->completed);

		blk_used_idx += 1;
	}

	spinlock_release(&blk_lock);
}
