#include <brk/assert.h>
#include <brk/net/eth.h>
#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/skbuff.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

u16 eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
	struct ethhdr *eth;
	u16 proto;

	(void)dev;

	if (skb->len < ETH_HLEN)
		return 0;

	eth = (struct ethhdr *)skb->data;
	proto = ntohs(eth->h_proto);
	skb_pull(skb, ETH_HLEN);
	return proto;
}

int eth_header(struct sk_buff *skb, struct net_device *dev, u16 type,
	       const u8 *daddr)
{
	struct ethhdr *eth;

	if (skb_headroom(skb) < ETH_HLEN)
		return -ENOSPC;

	eth = (struct ethhdr *)skb_push(skb, ETH_HLEN);
	memcpy(eth->h_dest, daddr ? daddr : eth_broadcast_addr(), ETH_ALEN);
	memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);
	eth->h_proto = htons(type);
	return 0;
}
