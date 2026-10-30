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
	u16 protocol; /* host byte order ETH_P_* */
	u16 flags;
	u32 len;
	u8 *head;
	u8 *data;
	u8 *tail;
	u8 *end;

	union {
		struct {
			u32 saddr;
			u32 daddr;
			u16 sport;
			u16 dport;
		} ip;
		struct {
			u8 type;
			u8 code;
		} icmp;
	} cb;
};

struct sk_buff *skb_alloc(size_t payload);
void skb_free(struct sk_buff *skb);

u8 *skb_put(struct sk_buff *skb, size_t len);
u8 *skb_push(struct sk_buff *skb, size_t len);
void skb_pull(struct sk_buff *skb, size_t len);
void skb_reserve(struct sk_buff *skb, size_t len);

static inline u32 skb_headroom(const struct sk_buff *skb)
{
	return (u32)(skb->data - skb->head);
}

static inline u32 skb_tailroom(const struct sk_buff *skb)
{
	return (u32)(skb->end - skb->tail);
}

/* Simple FIFO used by ARP and, later, socket receive queues. */
struct sk_buff_head {
	struct sk_buff *head;
	struct sk_buff *tail;
	u32 qlen;
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
