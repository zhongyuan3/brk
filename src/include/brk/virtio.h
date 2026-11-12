#ifndef BRK_VIRTIO_H
#define BRK_VIRTIO_H

#include <brk/device.h>
#include <brk/list.h>
#include <brk/types.h>
#include <brk/virtio_regs.h>

struct virtio_device {
	u64 phys_base;
	size_t size;
	u32 irq;
	u32 id;
	u8 *mem_base;
	struct list_head list;
	struct device dev;
};

extern struct bus_type virtio_bus;

void virtio_bus_init(void);

int virtio_dev_add(struct virtio_device *dev);
void virtio_dev_remove(struct virtio_device *dev);
struct virtio_device *virtio_dev_get(u32 id);

#endif
