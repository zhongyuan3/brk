#include <brk/ktime.h>
#include <brk/net/arp.h>
#include <brk/net/eth.h>
#include <brk/net/ip.h>
#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/skbuff.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

#define ARP_TABLE_SIZE 16
#define ARP_PENDING_MAX 4
#define ARP_REQUEST_INTERVAL_NS 1000000000ULL

enum arp_state {
	ARP_INCOMPLETE,
	ARP_REACHABLE,
};

struct arp_entry {
	bool in_use;
	u32 ip;
	u8 mac[ETH_ALEN];
	enum arp_state state;
	u64 last_request_ns;
	u64 last_used;
	struct sk_buff_head pending;
};

static struct arp_entry arp_table[ARP_TABLE_SIZE];
static SPINLOCK_DEFINE(arp_lock);
static u64 arp_generation;

static struct arp_entry *arp_lookup_locked(u32 ip)
{
	for (unsigned int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].in_use && arp_table[i].ip == ip)
			return &arp_table[i];
	}
	return NULL;
}

static struct arp_entry *arp_alloc_locked(u32 ip)
{
	struct arp_entry *victim = NULL;

	for (unsigned int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (!arp_table[i].in_use) {
			victim = &arp_table[i];
			break;
		}
	}

	if (!victim) {
		/* Evict the least recently used entry. */
		for (unsigned int i = 0; i < ARP_TABLE_SIZE; i++) {
			if (!victim ||
			    arp_table[i].last_used < victim->last_used)
				victim = &arp_table[i];
		}
		skb_queue_purge(&victim->pending);
		memset(victim, 0, sizeof(*victim));
		skb_queue_init(&victim->pending);
	}

	victim->in_use = true;
	victim->ip = ip;
	victim->state = ARP_INCOMPLETE;
	victim->last_used = ++arp_generation;
	return victim;
}

void arp_init(void)
{
	for (unsigned int i = 0; i < ARP_TABLE_SIZE; i++) {
		memset(&arp_table[i], 0, sizeof(arp_table[i]));
		skb_queue_init(&arp_table[i].pending);
	}
}

static int arp_send(struct net_device *dev, u16 op, const u8 *tha, u32 tpa)
{
	struct sk_buff *skb;
	struct arphdr *arp;
	int err;

	skb = skb_alloc(sizeof(*arp));
	if (!skb)
		return -ENOMEM;

	arp = (struct arphdr *)skb_put(skb, sizeof(*arp));
	arp->ar_hrd = htons(ARP_HWTYPE_ETHER);
	arp->ar_pro = htons(ETH_P_IP);
	arp->ar_hln = ETH_ALEN;
	arp->ar_pln = 4;
	arp->ar_op = htons(op);
	memcpy(arp->ar_sha, dev->dev_addr, ETH_ALEN);
	arp->ar_spa = dev->ip_addr;
	if (tha)
		memcpy(arp->ar_tha, tha, ETH_ALEN);
	else
		memset(arp->ar_tha, 0, ETH_ALEN);
	arp->ar_tpa = tpa;

	err = eth_header(skb, dev, ETH_P_ARP, tha ? tha : eth_broadcast_addr());
	if (err) {
		skb_free(skb);
		return err;
	}

	skb->dev = dev;
	return netdev_start_xmit(skb);
}

static void arp_flush_pending(const u8 *mac, struct sk_buff_head *queue)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(queue))) {
		struct net_device *out = skb->dev;

		if (!out || eth_header(skb, out, ETH_P_IP, mac)) {
			skb_free(skb);
			continue;
		}
		netdev_start_xmit(skb);
	}
}

static void arp_update(u32 ip, const u8 *mac, bool flush)
{
	struct sk_buff_head pending;
	struct arp_entry *e;
	irqstate_t flags;
	u8 hw[ETH_ALEN];
	bool do_flush = false;

	skb_queue_init(&pending);

	flags = spin_lock_irqsave(&arp_lock);
	e = arp_lookup_locked(ip);
	if (!e)
		e = arp_alloc_locked(ip);

	if (e) {
		memcpy(e->mac, mac, ETH_ALEN);
		e->state = ARP_REACHABLE;
		e->last_used = ++arp_generation;

		if (flush && !skb_queue_empty(&e->pending)) {
			memcpy(hw, mac, ETH_ALEN);
			do_flush = true;
			while (!skb_queue_empty(&e->pending))
				skb_queue_tail(&pending,
					       skb_dequeue(&e->pending));
		}
	}
	spin_unlock_irqrestore(&arp_lock, flags);

	if (do_flush)
		arp_flush_pending(hw, &pending);
}

int arp_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt)
{
	struct arphdr *arp;
	u32 spa, tpa;
	u16 op;
	u8 sha[ETH_ALEN];

	(void)pt;

	if (skb->len < sizeof(*arp))
		goto drop;

	arp = (struct arphdr *)skb->data;
	if (arp->ar_hrd != htons(ARP_HWTYPE_ETHER) ||
	    arp->ar_pro != htons(ETH_P_IP) || arp->ar_hln != ETH_ALEN ||
	    arp->ar_pln != 4)
		goto drop;

	op = ntohs(arp->ar_op);
	spa = arp->ar_spa;
	tpa = arp->ar_tpa;
	memcpy(sha, arp->ar_sha, ETH_ALEN);

	if (spa)
		arp_update(spa, sha, op == ARP_OP_REPLY);

	if (op == ARP_OP_REQUEST && tpa == dev->ip_addr)
		arp_send(dev, ARP_OP_REPLY, sha, spa);

	skb_free(skb);
	return 0;

drop:
	skb_free(skb);
	return -EINVAL;
}

int arp_output(struct sk_buff *skb, struct net_device *dev, u32 next_hop)
{
	struct arp_entry *e;
	irqstate_t flags;
	u64 now;
	bool send_request = false;
	u8 mac[ETH_ALEN];

	flags = spin_lock_irqsave(&arp_lock);

	e = arp_lookup_locked(next_hop);
	if (e && e->state == ARP_REACHABLE) {
		memcpy(mac, e->mac, ETH_ALEN);
		e->last_used = ++arp_generation;
		spin_unlock_irqrestore(&arp_lock, flags);

		skb->dev = dev;
		if (eth_header(skb, dev, ETH_P_IP, mac)) {
			skb_free(skb);
			return -ENOSPC;
		}
		return netdev_start_xmit(skb);
	}

	if (!e)
		e = arp_alloc_locked(next_hop);
	if (!e) {
		spin_unlock_irqrestore(&arp_lock, flags);
		skb_free(skb);
		return -ENOMEM;
	}

	skb->dev = dev;
	if (e->pending.qlen >= ARP_PENDING_MAX)
		skb_free(skb_dequeue(&e->pending));
	skb_queue_tail(&e->pending, skb);

	now = ktime_get_boot_ns();
	if (e->last_request_ns == 0 ||
	    now - e->last_request_ns >= ARP_REQUEST_INTERVAL_NS) {
		e->last_request_ns = now;
		send_request = true;
	}

	spin_unlock_irqrestore(&arp_lock, flags);

	if (send_request)
		arp_send(dev, ARP_OP_REQUEST, NULL, next_hop);

	return -EAGAIN;
}
