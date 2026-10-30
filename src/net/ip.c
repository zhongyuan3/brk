#include <brk/net/arp.h>
#include <brk/net/eth.h>
#include <brk/net/icmp.h>
#include <brk/net/ip.h>
#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/route.h>
#include <brk/net/skbuff.h>
#include <brk/net/tcp.h>
#include <brk/net/udp.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

static u16 ip_id;

int ip_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt)
{
	struct iphdr *iph;
	u32 ihl, tot_len, saddr, daddr;
	u8 protocol;

	(void)dev;
	(void)pt;

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

	/* Fragmentation/reassembly is not implemented. */
	if (ntohs(iph->frag_off) & (IP_MF | IP_OFFSET))
		goto drop;

	protocol = iph->protocol;
	saddr = iph->saddr;
	daddr = iph->daddr;

	skb->cb.ip.saddr = saddr;
	skb->cb.ip.daddr = daddr;

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
	int err;

	err = ip_route_output(daddr, &next_hop, &dev);
	if (err) {
		skb_free(skb);
		return err;
	}

	if (!saddr)
		saddr = dev->ip_addr;

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

	skb->dev = dev;
	skb->protocol = ETH_P_IP;

	if (dev->flags & IFF_LOOPBACK)
		return netdev_start_xmit(skb);

	return arp_output(skb, dev, next_hop);
}

int net_parse_ipv4(const char *buf, size_t len, u32 *addr)
{
	u32 parts[4] = { 0, 0, 0, 0 };
	int part = 0;
	bool digit = false;

	for (size_t i = 0; i < len; i++) {
		char c = buf[i];

		if (c >= '0' && c <= '9') {
			parts[part] = parts[part] * 10 + (u32)(c - '0');
			if (parts[part] > 255)
				return -EINVAL;
			digit = true;
		} else if (c == '.') {
			if (!digit || part >= 3)
				return -EINVAL;
			part++;
			digit = false;
		} else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			break;
		} else {
			return -EINVAL;
		}
	}

	if (!digit || part != 3)
		return -EINVAL;

	*addr = NET_IP4(parts[0], parts[1], parts[2], parts[3]);
	return 0;
}
