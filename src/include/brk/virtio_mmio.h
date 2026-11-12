#ifndef BRK_VIRTIO_MMIO_H
#define BRK_VIRTIO_MMIO_H

#include <brk/types.h>
#include <brk/virtio.h>

void virtio_mmio_platform_init(void);

struct virtio_device *virtio_mmio_dev_create(u64 phys_base, size_t size,
					     u32 irq);
void virtio_mmio_dev_destroy(struct virtio_device *dev);

#endif
