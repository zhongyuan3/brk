#include <assert.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/net/skbuff.h>
#include <brk/types.h>
#include <string.h>

struct sk_buff *skb_alloc(size_t payload)
{
	size_t total = sizeof(struct sk_buff) + SKB_HEADROOM + payload;
	struct sk_buff *skb;

	skb = kzalloc(total);
	if (!skb)
		return NULL;

	skb->head = (uint8_t *)(skb + 1);
	skb->data = skb->head + SKB_HEADROOM;
	skb->tail = skb->data;
	skb->end = skb->head + SKB_HEADROOM + payload;
	return skb;
}

void skb_free(struct sk_buff *skb)
{
	kfree(skb);
}

uint8_t *skb_put(struct sk_buff *skb, size_t len)
{
	uint8_t *old = skb->tail;

	assert(skb->tail + len <= skb->end);
	skb->tail += len;
	skb->len += len;
	return old;
}

uint8_t *skb_push(struct sk_buff *skb, size_t len)
{
	assert(skb->data - skb->head >= (ptrdiff_t)len);
	skb->data -= len;
	skb->len += len;
	return skb->data;
}

void skb_pull(struct sk_buff *skb, size_t len)
{
	assert(len <= skb->len);
	skb->data += len;
	skb->len -= len;
}

void skb_reserve(struct sk_buff *skb, size_t len)
{
	assert(len <= skb_tailroom(skb));
	skb->data += len;
	skb->tail += len;
}

void skb_queue_init(struct sk_buff_head *q)
{
	q->head = NULL;
	q->tail = NULL;
	q->qlen = 0;
}

void skb_queue_tail(struct sk_buff_head *q, struct sk_buff *skb)
{
	skb->next = NULL;
	if (q->tail)
		q->tail->next = skb;
	else
		q->head = skb;
	q->tail = skb;
	q->qlen++;
}

struct sk_buff *skb_dequeue(struct sk_buff_head *q)
{
	struct sk_buff *skb = q->head;

	if (!skb)
		return NULL;

	q->head = skb->next;
	if (!q->head)
		q->tail = NULL;
	skb->next = NULL;
	q->qlen--;
	return skb;
}

void skb_queue_purge(struct sk_buff_head *q)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(q)))
		skb_free(skb);
}
