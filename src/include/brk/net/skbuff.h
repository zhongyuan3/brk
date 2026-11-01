#ifndef BRK_NET_SKBUFF_H
#define BRK_NET_SKBUFF_H

#include <brk/types.h>

struct net_device;

#define SKB_HEADROOM 128

/*
 * A deliberately simple, linear sk_buff.  The header and the data live in
 * one kmalloc allocation; head/data/tail/end follow the usual layout:
 *
 *   head          data              tail            end
 *    |<headroom---|                 |<--- len ----->|
 *    |            |<---------- alloc size --------->|
 */
struct sk_buff {
	struct sk_buff *next;
	struct net_device *dev;
	uint16_t protocol; /* host byte order ETH_P_* */
	uint16_t flags;
	uint32_t len;
	uint8_t *head;
	uint8_t *data;
	uint8_t *tail;
	uint8_t *end;

	union {
		struct {
			uint32_t saddr;
			uint32_t daddr;
			uint16_t sport;
			uint16_t dport;
		} ip;
		struct {
			uint8_t type;
			uint8_t code;
		} icmp;
	} cb;
};

struct sk_buff *skb_alloc(size_t payload);
void skb_free(struct sk_buff *skb);

uint8_t *skb_put(struct sk_buff *skb, size_t len);
uint8_t *skb_push(struct sk_buff *skb, size_t len);
void skb_pull(struct sk_buff *skb, size_t len);
void skb_reserve(struct sk_buff *skb, size_t len);

static inline uint32_t skb_headroom(const struct sk_buff *skb)
{
	return (uint32_t)(skb->data - skb->head);
}

static inline uint32_t skb_tailroom(const struct sk_buff *skb)
{
	return (uint32_t)(skb->end - skb->tail);
}

/* Simple FIFO used by ARP and, later, socket receive queues. */
struct sk_buff_head {
	struct sk_buff *head;
	struct sk_buff *tail;
	uint32_t qlen;
};

void skb_queue_init(struct sk_buff_head *q);
void skb_queue_tail(struct sk_buff_head *q, struct sk_buff *skb);
struct sk_buff *skb_dequeue(struct sk_buff_head *q);
void skb_queue_purge(struct sk_buff_head *q);

static inline bool skb_queue_empty(const struct sk_buff_head *q)
{
	return !q->head;
}

#endif
