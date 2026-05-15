#ifndef BRK_VIRTIO_QUEUE_H
#define BRK_VIRTIO_QUEUE_H

#include <brk/types.h>

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

/* Support for indirect descriptors */
#define VIRTIO_F_INDIRECT_DESC 28

/* Support for avail_event and used_event fields */
#define VIRTIO_F_EVENT_IDX 29

/* Arbitrary descriptor layouts. */
#define VIRTIO_F_ANY_LAYOUT 27

/* Virtqueue descriptors: 16 bytes.
 * These can chain together via "next". */
struct virtq_desc {
	/* Address (guest-physical). */
	u64 addr;
	/* Length. */
	u32 len;
	/* The flags as indicated above. */
	u16 flags;
	/* We chain unused descriptors via this, too */
	u16 next;
};

struct virtq_avail {
	u16 flags;
	u16 idx;
	u16 ring[];
	/* Only if VIRTIO_F_EVENT_IDX: u16 used_event; */
};

/* u32 is used here for ids for padding reasons. */
struct virtq_used_elem {
	/* Index of start of used descriptor chain. */
	u32 id;
	/* Total length of the descriptor chain which was written to. */
	u32 len;
};

struct virtq_used {
	u16 flags;
	u16 idx;
	struct virtq_used_elem ring[];
	/* Only if VIRTIO_F_EVENT_IDX: u16 avail_event; */
};

struct virtq {
	unsigned int num;

	struct virtq_desc *desc;
	struct virtq_avail *avail;
	struct virtq_used *used;
};

static inline int virtq_need_event(u16 event_idx, u16 new_idx, u16 old_idx)
{
	return (u16)(new_idx - event_idx - 1) < (u16)(new_idx - old_idx);
}

/* Get location of event indices (only with VIRTIO_F_EVENT_IDX) */
static inline u16 *virtq_used_event(struct virtq *vq)
{
	/* For backwards compat, used event index is at *end* of avail ring. */
	return &vq->avail->ring[vq->num];
}

static inline u16 *virtq_avail_event(struct virtq *vq)
{
	/* For backwards compat, avail event index is at *end* of used ring. */
	return (u16 *)&vq->used->ring[vq->num];
}

#endif
