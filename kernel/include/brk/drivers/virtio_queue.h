#ifndef BRK_VIRTIO_QUEUE_H
#define BRK_VIRTIO_QUEUE_H

#include <brk/base/types.h>

/* This marks a buffer as continuing via the next field. */
#define VIRTQ_DESC_F_NEXT 1
/* This marks a buffer as write-only (otherwise read-only). */
#define VIRTQ_DESC_F_WRITE 2
/* This means the buffer contains a list of buffer descriptors. */
#define VIRTQ_DESC_F_INDIRECT 4

/* The device uses this in used->flags to advise the driver: don't kick me
 * when you add a buffer.  It's unreliable, so it's simply an
 * optimization. */
#define VIRTQ_USED_F_NO_NOTIFY 1
/* The driver uses this in avail->flags to advise the device: don't
 * interrupt me when you consume a buffer.  It's unreliable, so it's
 * simply an optimization.  */
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

/* Virtqueue descriptors: 16 bytes.
 * These can chain together via "next". */
struct virtq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct virtq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
};

struct virtq_used_elem {
	uint32_t id;
	uint32_t len;
};

struct virtq_used {
	uint16_t flags;
	uint16_t idx;
	struct virtq_used_elem ring[];
};

struct virtq {
	unsigned int num;
	struct virtq_desc *desc;
	struct virtq_avail *avail;
	struct virtq_used *used;
	bool *desc_used;
	uint16_t used_idx;
};

typedef void (*virtq_used_fn)(struct virtq *vq, uint32_t id, void *ctx);

int virtq_alloc(struct virtq *vq, unsigned int num);
void virtq_free(struct virtq *vq);
int virtq_alloc_desc(struct virtq *vq, unsigned int *idx);
int virtq_alloc_desc_chain(struct virtq *vq, unsigned int *idx,
			   unsigned int num);
void virtq_free_desc(struct virtq *vq, unsigned int idx);
void virtq_free_desc_chain(struct virtq *vq, unsigned int idx);
void virtq_submit(struct virtq *vq, uint16_t head_idx);
void virtq_process_used(struct virtq *vq, virtq_used_fn fn, void *ctx);

static inline int virtq_need_event(uint16_t event_idx, uint16_t new_idx,
				   uint16_t old_idx)
{
	return (uint16_t)(new_idx - event_idx - 1) <
	       (uint16_t)(new_idx - old_idx);
}

static inline uint16_t *virtq_used_event(struct virtq *vq)
{
	return &vq->avail->ring[vq->num];
}

static inline uint16_t *virtq_avail_event(struct virtq *vq)
{
	return (uint16_t *)&vq->used->ring[vq->num];
}

#endif
