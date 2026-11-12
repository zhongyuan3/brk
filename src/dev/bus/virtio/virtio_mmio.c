#include <arch/mm.h>
#include <arch/pgtable.h>
#include <brk/container_of.h>
#include <brk/device.h>
#include <brk/ioremap.h>
#include <brk/kmalloc.h>
#include <brk/mm.h>
#include <brk/mmio.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/virtio.h>
#include <brk/virtio_mmio.h>
#include <brk/virtio_queue.h>
#include <inttypes.h>
#include <uapi/errno.h>

struct virtio_mmio_device {
	struct virtio_device vdev;
	u64 phys_base;
	size_t size;
	u8 *mem_base;
};

static unsigned int virtio_dev_index;

static inline struct virtio_mmio_device *
to_virtio_mmio(struct virtio_device *vdev)
{
	return container_of(vdev, struct virtio_mmio_device, vdev);
}

static u64 virtio_mmio_base(struct virtio_device *vdev)
{
	return (uintptr_t)to_virtio_mmio(vdev)->mem_base;
}

static void virtio_mmio_write_addr(struct virtio_device *vdev, u32 low_off,
				   u32 high_off, u64 addr)
{
	u64 base = virtio_mmio_base(vdev);

	writel(addr & 0xffffffff, base + low_off);
	writel(addr >> 32, base + high_off);
}

static int virtio_mmio_probe(struct virtio_device *vdev)
{
	u64 base = virtio_mmio_base(vdev);

	if (readl(base + VIRTIO_MAGIC_VALUE_OFFSET) != VIRTIO_MAGIC_VALUE)
		return -EINVAL;

	if (readl(base + VIRTIO_VERSION_OFFSET) != VIRTIO_MMIO_VERSION)
		return -EINVAL;

	if (readl(base + VIRTIO_VENDOR_ID_OFFSET) != VIRTIO_VENDOR_ID)
		return -EINVAL;

	return 0;
}

static int virtio_mmio_set_status(struct virtio_device *vdev, u32 status)
{
	writel(status, virtio_mmio_base(vdev) + VIRTIO_STATUS_OFFSET);
	return 0;
}

static u32 virtio_mmio_get_status(struct virtio_device *vdev)
{
	return readl(virtio_mmio_base(vdev) + VIRTIO_STATUS_OFFSET);
}

static u32 virtio_mmio_get_features(struct virtio_device *vdev)
{
	return readl(virtio_mmio_base(vdev) + VIRTIO_DEVICE_FEATURES_OFFSET);
}

static int virtio_mmio_set_features(struct virtio_device *vdev, u32 features)
{
	writel(features,
	       virtio_mmio_base(vdev) + VIRTIO_DRIVER_FEATURES_OFFSET);
	return 0;
}

static int virtio_mmio_setup_queue(struct virtio_device *vdev, u32 queue_id,
				   struct virtq *vq, u32 queue_size)
{
	u64 base = virtio_mmio_base(vdev);
	u32 queue_size_max;

	writel(queue_id, base + VIRTIO_QUEUE_SEL_OFFSET);

	if (readl(base + VIRTIO_QUEUE_READY_OFFSET))
		panic("%s(): queue %" PRIu32 " should not be ready\n", __func__,
		      queue_id);

	queue_size_max = readl(base + VIRTIO_QUEUE_SIZE_MAX_OFFSET);
	if (queue_size_max == 0)
		panic("%s(): virtio device has no queue %" PRIu32 "\n",
		      __func__, queue_id);
	if (queue_size_max < queue_size)
		panic("%s(): virtio queue %" PRIu32 " max size %" PRIu32
		      " too short\n",
		      __func__, queue_id, queue_size_max);

	writel(queue_size, base + VIRTIO_QUEUE_SIZE_OFFSET);

	virtio_mmio_write_addr(vdev, VIRTIO_QUEUE_DESC_LOW_OFFSET,
			       VIRTIO_QUEUE_DESC_HIGH_OFFSET,
			       virt_to_phys((uintptr_t)vq->desc));
	virtio_mmio_write_addr(vdev, VIRTIO_QUEUE_DRIVER_LOW_OFFSET,
			       VIRTIO_QUEUE_DRIVER_HIGH_OFFSET,
			       virt_to_phys((uintptr_t)vq->avail));
	virtio_mmio_write_addr(vdev, VIRTIO_QUEUE_DEVICE_LOW_OFFSET,
			       VIRTIO_QUEUE_DEVICE_HIGH_OFFSET,
			       virt_to_phys((uintptr_t)vq->used));

	writel(1, base + VIRTIO_QUEUE_READY_OFFSET);
	return 0;
}

static void virtio_mmio_notify(struct virtio_device *vdev, u32 queue_id)
{
	writel(queue_id, virtio_mmio_base(vdev) + VIRTIO_QUEUE_NOTIFY_OFFSET);
}

static u32 virtio_mmio_ack_interrupt(struct virtio_device *vdev)
{
	u64 base = virtio_mmio_base(vdev);
	u32 status;

	status = readl(base + VIRTIO_INTERRUPT_STATUS_OFFSET) & 0x3;
	writel(status, base + VIRTIO_INTERRUPT_ACK_OFFSET);
	return status;
}

static int virtio_mmio_read_config(struct virtio_device *vdev, void *buf,
				   size_t size, u32 offset)
{
	struct virtio_mmio_device *mmio = to_virtio_mmio(vdev);
	u64 base;

	if (offset >= mmio->size || offset + size > mmio->size)
		return -EINVAL;

	base = virtio_mmio_base(vdev) + offset;
	for (size_t i = 0; i < size; ++i)
		((u8 *)buf)[i] = readb(base + i);

	return 0;
}

static const struct virtio_ops virtio_mmio_ops = {
	.set_status = virtio_mmio_set_status,
	.get_status = virtio_mmio_get_status,
	.get_features = virtio_mmio_get_features,
	.set_features = virtio_mmio_set_features,
	.setup_queue = virtio_mmio_setup_queue,
	.notify = virtio_mmio_notify,
	.ack_interrupt = virtio_mmio_ack_interrupt,
	.read_config = virtio_mmio_read_config,
};

struct virtio_device *virtio_mmio_dev_create(u64 phys_base, size_t size,
					     u32 irq)
{
	struct virtio_mmio_device *mmio;
	struct virtio_device *vdev;
	int err;

	mmio = kmalloc(sizeof(*mmio));
	if (!mmio)
		return NULL;

	mmio->phys_base = phys_base;
	mmio->size = size;
	mmio->mem_base = ioremap(phys_base, size, PTE_R | PTE_W);
	if (!mmio->mem_base) {
		kfree(mmio);
		return NULL;
	}

	vdev = &mmio->vdev;
	vdev->irq = irq;
	vdev->ops = &virtio_mmio_ops;

	err = virtio_mmio_probe(vdev);
	if (err) {
		iounmap(mmio->mem_base, mmio->size);
		kfree(mmio);
		return NULL;
	}

	vdev->id = readl(mmio->mem_base + VIRTIO_DEVICE_ID_OFFSET);

	char name[DEVICE_NAME_LEN];

	snprintf(name, sizeof(name), "virtio%u", virtio_dev_index++);
	device_init(&vdev->dev, name);
	vdev->dev.bus = &virtio_bus;
	return vdev;
}

void virtio_mmio_dev_destroy(struct virtio_device *vdev)
{
	struct virtio_mmio_device *mmio;

	if (!vdev)
		return;

	mmio = to_virtio_mmio(vdev);
	virtio_dev_remove(vdev);
	iounmap(mmio->mem_base, mmio->size);
	kfree(mmio);
}
