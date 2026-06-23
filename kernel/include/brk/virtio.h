#ifndef BRK_VIRTIO_H
#define BRK_VIRTIO_H

#include <brk/list.h>
#include <brk/types.h>
#include <brk/virtio_regs.h>

struct virtio_device {
	uint64_t phys_base;
	size_t size;
	uint32_t irq;
	uint32_t id;
	uint8_t *mem_base;
	struct list_head list;
};

struct virtio_device *virtio_dev_create(uint64_t phys_base, size_t size, uint32_t irq);
void virtio_dev_destroy(struct virtio_device *dev);
void virtio_dev_add(struct virtio_device *dev);
void virtio_dev_remove(struct virtio_device *dev);
struct virtio_device *virtio_dev_get(uint32_t id);
void virtio_init_scan(void);

#endif
