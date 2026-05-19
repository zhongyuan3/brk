#include <brk/errno.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/panic.h>
#include <brk/virtio.h>
#include <brk/virtio_mmio.h>
#include <uapi/errno.h>

static u64 virtio_mmio_base(struct virtio_device *dev)
{
	return (u64)dev->mem_base;
}

static void virtio_mmio_write_addr(struct virtio_device *dev, u32 low_off,
				   u32 high_off, u64 addr)
{
	u64 base = virtio_mmio_base(dev);

	writel(addr & 0xffffffff, base + low_off);
	writel(addr >> 32, base + high_off);
}

int virtio_mmio_probe(struct virtio_device *dev)
{
	u64 base = virtio_mmio_base(dev);

	if (readl(base + VIRTIO_MAGIC_VALUE_OFFSET) != VIRTIO_MAGIC_VALUE)
		return -EINVAL;

	if (readl(base + VIRTIO_VERSION_OFFSET) != VIRTIO_MMIO_VERSION)
		return -EINVAL;

	if (readl(base + VIRTIO_VENDOR_ID_OFFSET) != VIRTIO_VENDOR_ID)
		return -EINVAL;

	return 0;
}

void virtio_mmio_reset(struct virtio_device *dev)
{
	writel(0, virtio_mmio_base(dev) + VIRTIO_STATUS_OFFSET);
}

int virtio_mmio_start_driver(struct virtio_device *dev)
{
	u64 base = virtio_mmio_base(dev);
	u32 status;

	status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
	writel(status, base + VIRTIO_STATUS_OFFSET);
	return 0;
}

u32 virtio_mmio_read_features(struct virtio_device *dev)
{
	return readl(virtio_mmio_base(dev) + VIRTIO_DEVICE_FEATURES_OFFSET);
}

int virtio_mmio_write_features(struct virtio_device *dev, u32 features)
{
	writel(features, virtio_mmio_base(dev) + VIRTIO_DRIVER_FEATURES_OFFSET);
	return 0;
}

int virtio_mmio_features_ok(struct virtio_device *dev)
{
	u64 base = virtio_mmio_base(dev);
	u32 status;

	status = readl(base + VIRTIO_STATUS_OFFSET);
	status |= VIRTIO_STATUS_FEATURES_OK;
	writel(status, base + VIRTIO_STATUS_OFFSET);

	status = readl(base + VIRTIO_STATUS_OFFSET);
	if (!(status & VIRTIO_STATUS_FEATURES_OK))
		return -EOPNOTSUPP;

	return 0;
}

int virtio_mmio_setup_queue(struct virtio_device *dev, u32 queue_id,
			    struct virtq *vq, u32 queue_size)
{
	u64 base = virtio_mmio_base(dev);
	u32 queue_size_max;

	writel(queue_id, base + VIRTIO_QUEUE_SEL_OFFSET);

	if (readl(base + VIRTIO_QUEUE_READY_OFFSET))
		panic("%s(): queue %u should not be ready\n", __func__,
		      queue_id);

	queue_size_max = readl(base + VIRTIO_QUEUE_SIZE_MAX_OFFSET);
	if (queue_size_max == 0)
		panic("%s(): virtio device has no queue %u\n", __func__,
		      queue_id);
	if (queue_size_max < queue_size)
		panic("%s(): virtio queue %u max size %u too short\n", __func__,
		      queue_id, queue_size_max);

	writel(queue_size, base + VIRTIO_QUEUE_SIZE_OFFSET);

	virtio_mmio_write_addr(dev, VIRTIO_QUEUE_DESC_LOW_OFFSET,
			       VIRTIO_QUEUE_DESC_HIGH_OFFSET,
			       virt_to_phys((u64)vq->desc));
	virtio_mmio_write_addr(dev, VIRTIO_QUEUE_DRIVER_LOW_OFFSET,
			       VIRTIO_QUEUE_DRIVER_HIGH_OFFSET,
			       virt_to_phys((u64)vq->avail));
	virtio_mmio_write_addr(dev, VIRTIO_QUEUE_DEVICE_LOW_OFFSET,
			       VIRTIO_QUEUE_DEVICE_HIGH_OFFSET,
			       virt_to_phys((u64)vq->used));

	writel(1, base + VIRTIO_QUEUE_READY_OFFSET);
	return 0;
}

int virtio_mmio_driver_ok(struct virtio_device *dev)
{
	u64 base = virtio_mmio_base(dev);
	u32 status;

	status = readl(base + VIRTIO_STATUS_OFFSET);
	status |= VIRTIO_STATUS_DRIVER_OK;
	writel(status, base + VIRTIO_STATUS_OFFSET);
	return 0;
}

void virtio_mmio_queue_notify(struct virtio_device *dev, u32 queue_id)
{
	writel(queue_id, virtio_mmio_base(dev) + VIRTIO_QUEUE_NOTIFY_OFFSET);
}

u32 virtio_mmio_ack_interrupt(struct virtio_device *dev)
{
	u64 base = virtio_mmio_base(dev);
	u32 status;

	status = readl(base + VIRTIO_INTERRUPT_STATUS_OFFSET) & 0x3;
	writel(status, base + VIRTIO_INTERRUPT_ACK_OFFSET);
	return status;
}

int virtio_mmio_read(struct virtio_device *dev, void *buf, size_t size,
		     u32 offset)
{
	u64 base;

	if (offset >= dev->size || offset + size > dev->size)
		return -EINVAL;

	base = virtio_mmio_base(dev) + offset;
	for (size_t i = 0; i < size; ++i) {
		((u8 *)buf)[i] = readb(base + i);
	}

	return 0;
}
