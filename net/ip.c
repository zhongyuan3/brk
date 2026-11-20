#include <brk/arp.h>
#include <brk/eth.h>
#include <brk/icmp.h>
#include <brk/ip.h>
#include <brk/net.h>
#include <brk/netdevice.h>
#include <brk/route.h>
#include <brk/skbuff.h>
#include <brk/string.h>
#include <brk/tcp.h>
#include <brk/types.h>
#include <brk/udp.h>
#include <uapi/errno.h>

static u16 ip_id;

static int ip_transmit(struct sk_buff *skb, struct net_device *dev,
		       u32 next_hop, u32 daddr)
{
	skb->dev = dev;
	skb->protocol = ETH_P_IP;

	if (daddr == 0xffffffffU ||
	    (dev->broadcast && daddr == dev->broadcast)) {
		if (eth_header(skb, dev, ETH_P_IP, NULL)) {
			skb_free(skb);
			return -ENOSPC;
		}
		return netdev_start_xmit(skb);
	}

	if (dev->flags & IFF_LOOPBACK)
		return netdev_start_xmit(skb);

	return arp_output(skb, dev, next_hop);
}

int ip_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt)
{
	struct iphdr *iph;
	u32 ihl, tot_len, saddr, daddr;
	u8 protocol;

	(void)pt;

again:
	if (skb->len < sizeof(*iph))
		goto drop;

	iph = (struct iphdr *)skb->data;
	if (IP_VERSION(iph) != 4)
		goto drop;

	ihl = (u32)IP_IHL(iph) * 4;
	if (ihl < sizeof(*iph) || skb->len < ihl)
		goto drop;

	tot_len = ntohs(iph->tot_len);
	if (tot_len < ihl || tot_len > skb->len)
		goto drop;

	if (inet_csum(iph, ihl) != 0)
		goto drop;

	if (ntohs(iph->frag_off) & (IP_MF | IP_OFFSET)) {
		skb = ip_defrag(skb);
		if (!skb)
			return 0;
		goto again;
	}

	protocol = iph->protocol;
	saddr = iph->saddr;
	daddr = iph->daddr;

	skb->cb.ip.saddr = saddr;
	skb->cb.ip.daddr = daddr;
	skb->dev = dev;

	/* Drop Ethernet padding and expose only the IP payload. */
	skb->len = tot_len;
	skb->tail = skb->data + tot_len;
	skb_pull(skb, ihl);

	switch (protocol) {
	case IPPROTO_ICMP:
		return icmp_rcv(skb);
	case IPPROTO_UDP:
		return udp_rcv(skb);
	case IPPROTO_TCP:
		return tcp_rcv(skb);
	default:
		icmp_send_error(ICMP_DEST_UNREACH, ICMP_PROT_UNREACH, saddr,
				daddr, protocol, skb->data, skb->len);
		break;
	}

drop:
	skb_free(skb);
	return -EINVAL;
}

int ip_output(struct sk_buff *skb, u32 saddr, u32 daddr, u8 protocol, u8 ttl)
{
	struct iphdr *iph;
	struct net_device *dev;
	u32 next_hop;
	u32 payload_len;
	u32 max_payload;
	u32 off;
	u16 id;
	int err;

	err = ip_route_output(daddr, &next_hop, &dev);
	if (err) {
		skb_free(skb);
		return err;
	}

	if (!saddr)
		saddr = dev->ip_addr;

	payload_len = skb->len;
	if (payload_len + sizeof(*iph) > dev->mtu &&
	    !(dev->flags & IFF_LOOPBACK)) {
		max_payload = (dev->mtu - sizeof(*iph)) & ~7U;
		if (dev->mtu <= sizeof(*iph) || max_payload < 8) {
			skb_free(skb);
			return -EMSGSIZE;
		}

		id = ++ip_id;
		for (off = 0; off < payload_len; off += max_payload) {
			u32 chunk = payload_len - off;
			bool more;
			struct sk_buff *frag;

			if (chunk > max_payload)
				chunk = max_payload;
			more = (off + chunk) < payload_len;

			frag = skb_alloc(sizeof(*iph) + chunk);
			if (!frag) {
				skb_free(skb);
				return -ENOMEM;
			}

			iph = (struct iphdr *)skb_put(frag, sizeof(*iph));
			memset(iph, 0, sizeof(*iph));
			iph->version_ihl = 0x45;
			iph->tot_len = htons(sizeof(*iph) + chunk);
			iph->id = htons(id);
			iph->frag_off = htons((u16)((off >> 3) & IP_OFFSET) |
					      (more ? IP_MF : 0));
			iph->ttl = ttl ? ttl : IP_TTL_DEFAULT;
			iph->protocol = protocol;
			iph->saddr = saddr;
			iph->daddr = daddr;
			iph->check = inet_csum(iph, sizeof(*iph));
			memcpy(skb_put(frag, chunk), skb->data + off, chunk);

			err = ip_transmit(frag, dev, next_hop, daddr);
			if (err && err != -EAGAIN) {
				skb_free(skb);
				return err;
			}
		}

		skb_free(skb);
		return 0;
	}

	if (skb_headroom(skb) < sizeof(*iph)) {
		skb_free(skb);
		return -ENOSPC;
	}

	iph = (struct iphdr *)skb_push(skb, sizeof(*iph));
	memset(iph, 0, sizeof(*iph));
	iph->version_ihl = 0x45;
	iph->tot_len = htons(skb->len);
	iph->id = htons(++ip_id);
	iph->frag_off = htons(IP_DF);
	iph->ttl = ttl ? ttl : IP_TTL_DEFAULT;
	iph->protocol = protocol;
	iph->saddr = saddr;
	iph->daddr = daddr;
	iph->check = inet_csum(iph, sizeof(*iph));

	return ip_transmit(skb, dev, next_hop, daddr);
}
