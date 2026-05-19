#ifndef BRK_VIRTIO_MMIO_H
#define BRK_VIRTIO_MMIO_H

#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_queue.h>

int virtio_mmio_probe(struct virtio_device *dev);
void virtio_mmio_reset(struct virtio_device *dev);
int virtio_mmio_start_driver(struct virtio_device *dev);
u32 virtio_mmio_read_features(struct virtio_device *dev);
int virtio_mmio_write_features(struct virtio_device *dev, u32 features);
int virtio_mmio_features_ok(struct virtio_device *dev);
int virtio_mmio_setup_queue(struct virtio_device *dev, u32 queue_id,
			    struct virtq *vq, u32 queue_size);
int virtio_mmio_driver_ok(struct virtio_device *dev);
void virtio_mmio_queue_notify(struct virtio_device *dev, u32 queue_id);
u32 virtio_mmio_ack_interrupt(struct virtio_device *dev);
int virtio_mmio_read(struct virtio_device *dev, void *buf, size_t size, u32 offset);

#endif
