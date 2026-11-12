#ifndef BRK_VIRTIO_BLK_INTERNAL_H
#define BRK_VIRTIO_BLK_INTERNAL_H

#include <brk/irq.h>
#include <brk/types.h>
#include <brk/virtio_blk.h>

struct request;
struct request_queue;
struct virtio_device;

int virtio_blk_validate_dev(struct virtio_device *vdev);
u32 virtio_blk_select_features(struct virtio_device *vdev);
int virtio_blk_alloc_vq(struct virtio_blk_dev *vblk, unsigned int queue_size);
void virtio_blk_free_vq(struct virtio_blk_dev *vblk);
irqreturn_t virtio_blk_irq_handler(int irq, void *dev_id);
int virtio_blk_queue_rq(struct request_queue *q, struct request *rq);

#endif
