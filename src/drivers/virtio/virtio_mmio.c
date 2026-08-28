#include <arch/mm.h>
#include <brk/drivers/virtio.h>
#include <brk/drivers/virtio_mmio.h>
#include <brk/mm/mm.h>
#include <brk/mm/mmio.h>
#include <brk/printk/panic.h>
#include <uapi/brk/errno.h>

static uint64_t virtio_mmio_base(struct virtio_device *dev)
{
	return (uintptr_t)dev->mem_base;
}

static void virtio_mmio_write_addr(struct virtio_device *dev, uint32_t low_off,
				   uint32_t high_off, uint64_t addr)
{
	uint64_t base = virtio_mmio_base(dev);

	writel(addr & 0xffffffff, base + low_off);
	writel(addr >> 32, base + high_off);
}

int virtio_mmio_probe(struct virtio_device *dev)
{
	uint64_t base = virtio_mmio_base(dev);

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
	uint64_t base = virtio_mmio_base(dev);
	uint32_t status;

	status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
	writel(status, base + VIRTIO_STATUS_OFFSET);
	return 0;
}

uint32_t virtio_mmio_read_features(struct virtio_device *dev)
{
	return readl(virtio_mmio_base(dev) + VIRTIO_DEVICE_FEATURES_OFFSET);
}

int virtio_mmio_write_features(struct virtio_device *dev, uint32_t features)
{
	writel(features, virtio_mmio_base(dev) + VIRTIO_DRIVER_FEATURES_OFFSET);
	return 0;
}

int virtio_mmio_features_ok(struct virtio_device *dev)
{
	uint64_t base = virtio_mmio_base(dev);
	uint32_t status;

	status = readl(base + VIRTIO_STATUS_OFFSET);
	status |= VIRTIO_STATUS_FEATURES_OK;
	writel(status, base + VIRTIO_STATUS_OFFSET);

	status = readl(base + VIRTIO_STATUS_OFFSET);
	if (!(status & VIRTIO_STATUS_FEATURES_OK))
		return -EOPNOTSUPP;

	return 0;
}

int virtio_mmio_setup_queue(struct virtio_device *dev, uint32_t queue_id,
			    struct virtq *vq, uint32_t queue_size)
{
	uint64_t base = virtio_mmio_base(dev);
	uint32_t queue_size_max;

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
			       virt_to_phys((uintptr_t)vq->desc));
	virtio_mmio_write_addr(dev, VIRTIO_QUEUE_DRIVER_LOW_OFFSET,
			       VIRTIO_QUEUE_DRIVER_HIGH_OFFSET,
			       virt_to_phys((uintptr_t)vq->avail));
	virtio_mmio_write_addr(dev, VIRTIO_QUEUE_DEVICE_LOW_OFFSET,
			       VIRTIO_QUEUE_DEVICE_HIGH_OFFSET,
			       virt_to_phys((uintptr_t)vq->used));

	writel(1, base + VIRTIO_QUEUE_READY_OFFSET);
	return 0;
}

int virtio_mmio_driver_ok(struct virtio_device *dev)
{
	uint64_t base = virtio_mmio_base(dev);
	uint32_t status;

	status = readl(base + VIRTIO_STATUS_OFFSET);
	status |= VIRTIO_STATUS_DRIVER_OK;
	writel(status, base + VIRTIO_STATUS_OFFSET);
	return 0;
}

void virtio_mmio_queue_notify(struct virtio_device *dev, uint32_t queue_id)
{
	writel(queue_id, virtio_mmio_base(dev) + VIRTIO_QUEUE_NOTIFY_OFFSET);
}

uint32_t virtio_mmio_ack_interrupt(struct virtio_device *dev)
{
	uint64_t base = virtio_mmio_base(dev);
	uint32_t status;

	status = readl(base + VIRTIO_INTERRUPT_STATUS_OFFSET) & 0x3;
	writel(status, base + VIRTIO_INTERRUPT_ACK_OFFSET);
	return status;
}

int virtio_mmio_read(struct virtio_device *dev, void *buf, size_t size,
		     uint32_t offset)
{
	uint64_t base;

	if (offset >= dev->size || offset + size > dev->size)
		return -EINVAL;

	base = virtio_mmio_base(dev) + offset;
	for (size_t i = 0; i < size; ++i) {
		((uint8_t *)buf)[i] = readb(base + i);
	}

	return 0;
}
