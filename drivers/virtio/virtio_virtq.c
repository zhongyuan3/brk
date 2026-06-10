#include <brk/drivers/virtio_queue.h>
#include <brk/lib/assert.h>
#include <brk/lib/kernel.h>
#include <brk/mm/kmalloc.h>
#include <uapi/brk/errno.h>

int virtq_alloc(struct virtq *vq, unsigned int num)
{
	usize_t size;

	vq->desc = kcalloc(num, sizeof(struct virtq_desc));
	if (!vq->desc)
		return -ENOMEM;

	size = sizeof(struct virtq_avail) + sizeof(u16) * num;
	vq->avail = kzalloc(size);
	if (!vq->avail)
		goto err_desc;

	size = sizeof(struct virtq_used) + sizeof(struct virtq_used_elem) * num;
	vq->used = kzalloc(size);
	if (!vq->used)
		goto err_avail;

	vq->desc_used = kcalloc(num, sizeof(bool));
	if (!vq->desc_used)
		goto err_used;

	vq->num = num;
	vq->used_idx = 0;
	return 0;

err_used:
	kfree(vq->used);
	vq->used = NULL;
err_avail:
	kfree(vq->avail);
	vq->avail = NULL;
err_desc:
	kfree(vq->desc);
	vq->desc = NULL;
	return -ENOMEM;
}

void virtq_free(struct virtq *vq)
{
	kfree(vq->desc_used);
	kfree(vq->used);
	kfree(vq->avail);
	kfree(vq->desc);
	vq->desc = NULL;
	vq->avail = NULL;
	vq->used = NULL;
	vq->desc_used = NULL;
	vq->num = 0;
	vq->used_idx = 0;
}

int virtq_alloc_desc(struct virtq *vq, unsigned int *idx)
{
	for (unsigned int i = 0; i < vq->num; i++) {
		if (!vq->desc_used[i]) {
			vq->desc_used[i] = true;
			*idx = i;
			return 0;
		}
	}
	return -ENOMEM;
}

void virtq_free_desc(struct virtq *vq, unsigned int idx)
{
	ASSERT(idx < vq->num);
	ASSERT(vq->desc_used[idx]);
	vq->desc_used[idx] = false;
	vq->desc[idx].addr = 0;
	vq->desc[idx].len = 0;
	vq->desc[idx].flags = 0;
	vq->desc[idx].next = 0;
}

void virtq_free_desc_chain(struct virtq *vq, unsigned int idx)
{
	while (1) {
		unsigned int flags = vq->desc[idx].flags;
		unsigned int next = vq->desc[idx].next;

		virtq_free_desc(vq, idx);
		if (flags & VIRTQ_DESC_F_NEXT)
			idx = next;
		else
			break;
	}
}

int virtq_alloc_desc_chain(struct virtq *vq, unsigned int *idx,
			   unsigned int num)
{
	for (unsigned int i = 0; i < num; i++) {
		if (virtq_alloc_desc(vq, idx + i)) {
			for (unsigned int j = 0; j < i; j++)
				virtq_free_desc(vq, idx[j]);
			return -ENOMEM;
		}
	}
	return 0;
}

void virtq_submit(struct virtq *vq, u16 head_idx)
{
	vq->avail->ring[vq->avail->idx % vq->num] = head_idx;

	__sync_synchronize();

	vq->avail->idx += 1;

	__sync_synchronize();
}

void virtq_process_used(struct virtq *vq, virtq_used_fn fn, void *ctx)
{
	u16 curr_used_idx = vq->used->idx;

	while (vq->used_idx != curr_used_idx) {
		u32 id;

		__sync_synchronize();
		id = vq->used->ring[vq->used_idx % vq->num].id;
		fn(vq, id, ctx);
		vq->used_idx += 1;
	}
}
