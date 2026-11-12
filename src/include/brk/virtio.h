#ifndef BRK_VIRTIO_H
#define BRK_VIRTIO_H

#include <brk/device.h>
#include <brk/types.h>
#include <brk/virtio_regs.h>

struct virtq;

struct virtio_device;

struct virtio_ops {
	int (*set_status)(struct virtio_device *vdev, u32 status);
	u32 (*get_status)(struct virtio_device *vdev);
	u32 (*get_features)(struct virtio_device *vdev);
	int (*set_features)(struct virtio_device *vdev, u32 features);
	int (*setup_queue)(struct virtio_device *vdev, u32 queue_id,
			   struct virtq *vq, u32 queue_size);
	void (*notify)(struct virtio_device *vdev, u32 queue_id);
	u32 (*ack_interrupt)(struct virtio_device *vdev);
	int (*read_config)(struct virtio_device *vdev, void *buf, size_t size,
			   u32 offset);
};

struct virtio_device {
	u32 id;
	u32 irq;
	const struct virtio_ops *ops;
	struct device dev;
};

extern struct bus_type virtio_bus;

void virtio_bus_init(void);

int virtio_dev_add(struct virtio_device *dev);
void virtio_dev_remove(struct virtio_device *dev);

/* Transport-independent virtio feature and status operations. */
void virtio_reset(struct virtio_device *vdev);
int virtio_start_driver(struct virtio_device *vdev);
u32 virtio_read_features(struct virtio_device *vdev);
int virtio_write_features(struct virtio_device *vdev, u32 features);
int virtio_features_ok(struct virtio_device *vdev);
int virtio_setup_queue(struct virtio_device *vdev, u32 queue_id,
		       struct virtq *vq, u32 queue_size);
int virtio_driver_ok(struct virtio_device *vdev);
void virtio_queue_notify(struct virtio_device *vdev, u32 queue_id);
u32 virtio_ack_interrupt(struct virtio_device *vdev);
int virtio_read_config(struct virtio_device *vdev, void *buf, size_t size,
		       u32 offset);

#endif
