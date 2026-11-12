#include <brk/ip.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/skbuff.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

#define IP_FRAG_QUEUES 8
#define IP_FRAG_TIMEOUT_NS 30000000000ULL
#define IP_FRAG_MAX_PAYLOAD (65535U - sizeof(struct iphdr))

struct ip_frag_piece {
	struct ip_frag_piece *next;
	u32 offset;
	u32 len;
	u8 data[];
};

struct ip_frag_queue {
	bool in_use;
	u32 saddr;
	u32 daddr;
	u16 id;
	u8 protocol;
	u8 header[60];
	u32 header_len;
	u32 total_len; /* 0 until the final fragment is seen. */
	u32 received;
	u64 last_seen_ns;
	struct ip_frag_piece *pieces;
};

static struct ip_frag_queue frag_queues[IP_FRAG_QUEUES];
static SPINLOCK_DEFINE(ip_frag_lock);

static void ip_frag_free_queue_locked(struct ip_frag_queue *q)
{
	struct ip_frag_piece *p = q->pieces;

	while (p) {
		struct ip_frag_piece *next = p->next;

		kfree(p);
		p = next;
	}
	memset(q, 0, sizeof(*q));
}

static void ip_frag_expire_locked(u64 now)
{
	for (unsigned int i = 0; i < IP_FRAG_QUEUES; i++) {
		struct ip_frag_queue *q = &frag_queues[i];

		if (q->in_use && now - q->last_seen_ns >= IP_FRAG_TIMEOUT_NS)
			ip_frag_free_queue_locked(q);
	}
}

static struct ip_frag_queue *ip_frag_find_locked(u32 saddr, u32 daddr, u16 id,
						 u8 protocol)
{
	for (unsigned int i = 0; i < IP_FRAG_QUEUES; i++) {
		struct ip_frag_queue *q = &frag_queues[i];

		if (!q->in_use)
			continue;
		if (q->saddr == saddr && q->daddr == daddr && q->id == id &&
		    q->protocol == protocol)
			return q;
	}
	return NULL;
}

static struct ip_frag_queue *ip_frag_alloc_locked(u32 saddr, u32 daddr, u16 id,
						  u8 protocol)
{
	struct ip_frag_queue *victim = NULL;

	for (unsigned int i = 0; i < IP_FRAG_QUEUES; i++) {
		if (!frag_queues[i].in_use) {
			victim = &frag_queues[i];
			break;
		}
	}

	if (!victim) {
		for (unsigned int i = 0; i < IP_FRAG_QUEUES; i++) {
			if (!victim ||
			    frag_queues[i].last_seen_ns < victim->last_seen_ns)
				victim = &frag_queues[i];
		}
		ip_frag_free_queue_locked(victim);
	}

	memset(victim, 0, sizeof(*victim));
	victim->in_use = true;
	victim->saddr = saddr;
	victim->daddr = daddr;
	victim->id = id;
	victim->protocol = protocol;
	return victim;
}

static bool ip_frag_overlaps(struct ip_frag_queue *q, u32 offset, u32 len)
{
	struct ip_frag_piece *p;

	for (p = q->pieces; p; p = p->next) {
		if (offset < p->offset + p->len && p->offset < offset + len)
			return true;
	}
	return false;
}

struct sk_buff *ip_defrag(struct sk_buff *skb)
{
	struct iphdr *iph = (struct iphdr *)skb->data;
	struct ip_frag_piece *piece;
	struct ip_frag_queue *q;
	struct sk_buff *out = NULL;
	u32 ihl = (u32)IP_IHL(iph) * 4;
	u32 tot_len = ntohs(iph->tot_len);
	u32 payload_len = tot_len - ihl;
	u16 frag_off = ntohs(iph->frag_off);
	u32 offset = (u32)(frag_off & IP_OFFSET) * 8;
	bool more = (frag_off & IP_MF) != 0;
	irqstate_t flags;
	u64 now;

	if (payload_len == 0 || offset + payload_len > IP_FRAG_MAX_PAYLOAD)
		goto drop;

	piece = kmalloc(sizeof(*piece) + payload_len);
	if (!piece)
		goto drop;
	piece->next = NULL;
	piece->offset = offset;
	piece->len = payload_len;
	memcpy(piece->data, skb->data + ihl, payload_len);

	now = ktime_boottime_ns();
	flags = spin_lock_irqsave(&ip_frag_lock);
	ip_frag_expire_locked(now);

	q = ip_frag_find_locked(iph->saddr, iph->daddr, ntohs(iph->id),
				iph->protocol);
	if (offset == 0) {
		if (q || ihl > sizeof(q->header))
			goto unlock_drop;
		q = ip_frag_alloc_locked(iph->saddr, iph->daddr, ntohs(iph->id),
					 iph->protocol);
		if (!q)
			goto unlock_drop;
		memcpy(q->header, iph, ihl);
		q->header_len = ihl;
	} else if (!q) {
		goto unlock_drop;
	}

	if (!more) {
		if (q->total_len && q->total_len != offset + payload_len)
			goto unlock_drop;
		q->total_len = offset + payload_len;
	}
	if (q->total_len && offset + payload_len > q->total_len)
		goto unlock_drop;
	if (ip_frag_overlaps(q, offset, payload_len))
		goto unlock_drop;

	struct ip_frag_piece **pp = &q->pieces;

	while (*pp && (*pp)->offset < offset)
		pp = &(*pp)->next;
	piece->next = *pp;
	*pp = piece;
	piece = NULL;
	q->received += payload_len;
	q->last_seen_ns = now;

	if (!q->total_len || q->received != q->total_len)
		goto unlock_done;

	u32 total = q->header_len + q->total_len;

	if (total > 65535) {
		ip_frag_free_queue_locked(q);
		goto unlock_done;
	}

	out = skb_alloc(total);
	if (!out)
		goto unlock_done;

	u8 *dst = skb_put(out, total);

	memcpy(dst, q->header, q->header_len);
	u32 pos = q->header_len;
	struct ip_frag_piece *p;

	for (p = q->pieces; p; p = p->next) {
		memcpy(dst + pos, p->data, p->len);
		pos += p->len;
	}

	struct iphdr *out_iph = (struct iphdr *)dst;

	out_iph->tot_len = htons(total);
	out_iph->frag_off = 0;
	out_iph->check = 0;
	out_iph->check = inet_csum(out_iph, q->header_len);
	out->dev = skb->dev;
	ip_frag_free_queue_locked(q);

unlock_done:
	spin_unlock_irqrestore(&ip_frag_lock, flags);
	kfree(piece);
	skb_free(skb);
	return out;

unlock_drop:
	spin_unlock_irqrestore(&ip_frag_lock, flags);
	kfree(piece);
drop:
	skb_free(skb);
	return NULL;
}
