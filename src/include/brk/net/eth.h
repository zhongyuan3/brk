#ifndef BRK_NET_ETH_H
#define BRK_NET_ETH_H

#include <brk/net/netdevice.h>
#include <brk/net/skbuff.h>
#include <brk/types.h>

#define ETH_ALEN 6
#define ETH_HLEN 14

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_IPV6 0x86dd
#define ETH_P_ALL 0x0003

struct ethhdr {
	u8 h_dest[ETH_ALEN];
	u8 h_source[ETH_ALEN];
	u16 h_proto;
} __attribute__((packed));

/* Pull the Ethernet header and return the protocol in host byte order. */
u16 eth_type_trans(struct sk_buff *skb, struct net_device *dev);

/* Push an Ethernet header; @daddr may be NULL for broadcast. */
int eth_header(struct sk_buff *skb, struct net_device *dev, u16 type,
	       const u8 *daddr);

static inline const u8 *eth_broadcast_addr(void)
{
	static const u8 addr[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	return addr;
}

#endif
