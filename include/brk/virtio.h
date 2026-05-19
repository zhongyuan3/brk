#ifndef BRK_VIRTIO_H
#define BRK_VIRTIO_H

#include <brk/list.h>
#include <brk/types.h>
#include <brk/virtio_regs.h>

struct virtio_device {
	u64 phys_base;
	usize_t size;
	u32 irq;
	u32 id;
	u8 *mem_base;
	struct list_head list;
};

struct virtio_device *virtio_dev_create(u64 phys_base, usize_t size, u32 irq);
void virtio_dev_destroy(struct virtio_device *dev);
void virtio_dev_add(struct virtio_device *dev);
void virtio_dev_remove(struct virtio_device *dev);
struct virtio_device *virtio_dev_get(u32 id);
void virtio_init_scan(void);

#endif
