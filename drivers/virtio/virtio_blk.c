#include <aosd/align.h>
#include <aosd/assert.h>
#include <aosd/errno.h>
#include <aosd/irq.h>
#include <aosd/mm.h>
#include <aosd/mmio.h>
#include <aosd/panic.h>
#include <aosd/plic.h>
#include <aosd/printk.h>
#include <aosd/sched.h>
#include <aosd/slab.h>
#include <aosd/virtio.h>
#include <aosd/virtio_blk.h>
#include <aosd/virtio_queue.h>

static struct virtio_device *blk_dev;
static struct virtq blk_vq;
static struct virtio_blk_req *blk_reqs;
static struct virtio_blk_track *blk_tracks;
static struct kmem_cache blk_trans_cache;
static uint16_t used_idx;

static void virtio_blk_irq_handler(void)
{
	uint32_t status;
	int id;
	struct virtio_blk_transation *trans;
	uint64_t mem_base = (uint64_t)blk_dev->mem_base;

	status = readl(mem_base + VIRTIO_INTERRUPT_STATUS_OFFSET) & 0x3;
	writel(status, mem_base + VIRTIO_INTERRUPT_ACK_OFFSET);

	__sync_synchronize();

	while (used_idx != blk_vq.used->idx) {
		__sync_synchronize();
		id = blk_vq.used->ring[used_idx % blk_vq.num].id;

		if (blk_tracks[id].status != 0) {
			panic("%s(): invalid track status %d\n", __func__,
			      blk_tracks[id].status);
		}

		trans = blk_tracks[id].trans;
		trans->completed = true;
		sched_wake_up(&trans->completed);

		++used_idx;
	}
}

int virtio_blk_init(struct virtio_device *dev, unsigned int queue_size)
{
	uint64_t mem_base;
	uint32_t status;
	uint64_t features;
	int err;
	uint64_t paddr;
	uint32_t queue_max_size;
	size_t size;

	if (dev->id != VIRTIO_DEVICE_ID_BLK) {
		log_debug("%s(): invalid device id %#x\n", __func__, dev->id);
		return -EINVAL;
	}

	mem_base = (uint64_t)dev->mem_base;

	if (readl(mem_base + VIRTIO_MAGIC_VALUE_OFFSET) != VIRTIO_MAGIC_VALUE) {
		log_debug("%s(): invalid magic value %#x\n", __func__,
			  readl(mem_base + VIRTIO_MAGIC_VALUE_OFFSET));
		return -EINVAL;
	}

	if (readl(mem_base + VIRTIO_VERSION_OFFSET) != 2) {
		log_debug("%s(): invalid version %#x\n", __func__,
			  readl(mem_base + VIRTIO_VERSION_OFFSET));
		return -EINVAL;
	}

	if (readl(mem_base + VIRTIO_VENDOR_ID_OFFSET) != VIRTIO_VENDOR_ID) {
		log_debug("%s(): invalid vendor id %#x\n", __func__,
			  readl(mem_base + VIRTIO_VENDOR_ID_OFFSET));
		return -EINVAL;
	}

	queue_max_size = readl(mem_base + VIRTIO_QUEUE_SIZE_MAX_OFFSET);
	if (queue_max_size < queue_size) {
		log_debug("%s(): queue size %u is too large, max size is %u",
			  __func__, queue_size, queue_max_size);
		return -EINVAL;
	}

	blk_vq.desc = kcalloc(queue_size, sizeof(struct virtq_desc));
	if (!blk_vq.desc) {
		log_debug("%s(): failed to allocate desc", __func__);
		goto alloc_desc_failed;
	}

	size = sizeof(struct virtq_avail) + sizeof(uint16_t) * queue_size;
	blk_vq.avail = kzalloc(size);
	if (!blk_vq.avail) {
		log_debug("%s(): failed to allocate avail", __func__);
		goto alloc_avail_failed;
	}

	size = sizeof(struct virtq_used) +
	       sizeof(struct virtq_used_elem) * queue_size;
	blk_vq.used = kzalloc(size);
	if (!blk_vq.used) {
		log_debug("%s(): failed to allocate used", __func__);
		goto alloc_used_failed;
	}

	blk_reqs = kcalloc(queue_size, sizeof(struct virtio_blk_req));
	if (!blk_reqs) {
		log_debug("%s(): failed to allocate reqs", __func__);
		goto alloc_reqs_failed;
	}

	blk_tracks = kcalloc(queue_size, sizeof(struct virtio_blk_track));
	if (!blk_tracks) {
		log_debug("%s(): failed to allocate tracks", __func__);
		goto alloc_tracks_failed;
	}

	err = kmem_cache_init(&blk_trans_cache,
			      sizeof(struct virtio_blk_transation),
			      alignof(struct virtio_blk_transation),
			      "blk_trans_cache");
	if (err) {
		log_debug(
			"%s(): failed to initialize blk_trans_cache, err %d\n",
			__func__, err);
		goto init_trans_cache_failed;
	}

	blk_vq.num = queue_size;

	status = 0;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);
	status |= VIRTIO_STATUS_ACKNOWLEDGE;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);
	status |= VIRTIO_STATUS_DRIVER;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	writel(0, mem_base + VIRTIO_DEVICE_FEATURES_SEL_OFFSET);
	writel(0, mem_base + VIRTIO_DRIVER_FEATURES_SEL_OFFSET);

	features = readl(mem_base + VIRTIO_DEVICE_FEATURES_OFFSET);
	features &= ~(1 << VIRTIO_BLK_F_RO);
	features &= ~(1 << VIRTIO_BLK_F_SCSI);
	features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
	features &= ~(1 << VIRTIO_BLK_F_MQ);
	features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
	features &= ~(1 << VIRTIO_F_EVENT_IDX);
	features &= ~(1 << VIRTIO_F_INDIRECT_DESC);
	writel(features, mem_base + VIRTIO_DRIVER_FEATURES_OFFSET);

	status |= VIRTIO_STATUS_FEATURES_OK;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	status = readl(mem_base + VIRTIO_STATUS_OFFSET);
	assert(status & VIRTIO_STATUS_FEATURES_OK);

	writel(0, mem_base + VIRTIO_QUEUE_SEL_OFFSET);
	assert(!readl(mem_base + VIRTIO_QUEUE_READY_OFFSET));

	writel(queue_size, mem_base + VIRTIO_QUEUE_SIZE_OFFSET);

	paddr = virt_to_phys((uint64_t)blk_vq.desc);
	writel(paddr, mem_base + VIRTIO_QUEUE_DESC_LOW_OFFSET);
	writel(paddr >> 32, mem_base + VIRTIO_QUEUE_DESC_HIGH_OFFSET);

	paddr = virt_to_phys((uint64_t)blk_vq.avail);
	writel(paddr, mem_base + VIRTIO_QUEUE_DRIVER_LOW_OFFSET);
	writel(paddr >> 32, mem_base + VIRTIO_QUEUE_DRIVER_HIGH_OFFSET);

	paddr = virt_to_phys((uint64_t)blk_vq.used);
	writel(paddr, mem_base + VIRTIO_QUEUE_DEVICE_LOW_OFFSET);
	writel(paddr >> 32, mem_base + VIRTIO_QUEUE_DEVICE_HIGH_OFFSET);

	writel(1, mem_base + VIRTIO_QUEUE_READY_OFFSET);

	status |= VIRTIO_STATUS_DRIVER_OK;
	writel(status, mem_base + VIRTIO_STATUS_OFFSET);

	blk_dev = dev;

	irq_register_handler(blk_dev->irq, virtio_blk_irq_handler, NULL);
	plic_set_priority(blk_dev->irq, 1);
	plic_enable(my_cpu(), blk_dev->irq);

	return 0;

init_trans_cache_failed:
	kfree(blk_tracks);
	blk_tracks = NULL;
alloc_tracks_failed:
	kfree(blk_reqs);
	blk_reqs = NULL;
alloc_reqs_failed:
	kfree(blk_vq.used);
	blk_vq.used = NULL;
alloc_used_failed:
	kfree(blk_vq.avail);
	blk_vq.avail = NULL;
alloc_avail_failed:
	kfree(blk_vq.desc);
	blk_vq.desc = NULL;
alloc_desc_failed:
	return -ENOMEM;
}

static int alloc_desc(unsigned int *desc_idx)
{
	for (unsigned int i = 0; i < blk_vq.num; i++) {
		if (!blk_vq.desc[i].addr) {
			blk_vq.desc[i].addr = 1;
			*desc_idx = i;
			return 0;
		}
	}
	return -1;
}

static void free_desc(unsigned int desc_idx)
{
	blk_vq.desc[desc_idx].addr = 0;
	blk_vq.desc[desc_idx].len = 0;
	blk_vq.desc[desc_idx].flags = 0;
	blk_vq.desc[desc_idx].next = 0;
}

static void free_desc_chain(unsigned int desc_idx)
{
	unsigned int next;
	while (desc_idx != 0) {
		next = blk_vq.desc[desc_idx].next;
		free_desc(desc_idx);
		desc_idx = next;
	}
}

static int alloc_desc_chain(unsigned int *desc_idx, unsigned int num)
{
	for (unsigned int i = 0; i < num; i++) {
		if (alloc_desc(desc_idx + i)) {
			for (unsigned int j = 0; j < i; j++)
				free_desc(desc_idx[j]);
			return -1;
		}
	}
	return 0;
}

static int virtio_blk_transfer(struct virtio_blk_transation *trans)
{
	unsigned int idx[3];
	struct virtio_blk_req *req;
	char *status;

	while (1) {
		if (alloc_desc_chain(idx, 3) == 0)
			break;
		sched_sleep(&blk_vq.desc);
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

	blk_vq.desc[idx[1]].addr = virt_to_phys((uint64_t)trans->buf);
	blk_vq.desc[idx[1]].len = trans->sec_count * 512;
	blk_vq.desc[idx[1]].flags = trans->is_write ? 0 : VIRTQ_DESC_F_WRITE;
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
	blk_vq.avail->idx++;
	__sync_synchronize();

	writel(0, blk_dev->mem_base + VIRTIO_QUEUE_NOTIFY_OFFSET);

	while (!trans->completed)
		sched_sleep(&trans->completed);

	blk_tracks[idx[0]].trans = NULL;
	free_desc_chain(idx[0]);

	return 0;
}

int virtio_blk_read(uint64_t sector, void *buf, size_t sec_count)
{
	struct virtio_blk_transation *trans;
	int err;

	trans = kmem_cache_alloc(&blk_trans_cache);
	if (!trans)
		return -ENOMEM;
	trans->buf = buf;
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

int virtio_blk_write(uint64_t sector, const void *buf, size_t sec_count)
{
	struct virtio_blk_transation *trans;
	int err;

	trans = kmem_cache_alloc(&blk_trans_cache);
	if (!trans)
		return -ENOMEM;
	trans->buf = (void *)buf;
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
