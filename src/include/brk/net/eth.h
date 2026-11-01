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
	uint8_t h_dest[ETH_ALEN];
	uint8_t h_source[ETH_ALEN];
	uint16_t h_proto;
} __attribute__((packed));

/* Pull the Ethernet header and return the protocol in host byte order. */
uint16_t eth_type_trans(struct sk_buff *skb, struct net_device *dev);

/* Push an Ethernet header; @daddr may be NULL for broadcast. */
int eth_header(struct sk_buff *skb, struct net_device *dev, uint16_t type,
	       const uint8_t *daddr);

static inline const uint8_t *eth_broadcast_addr(void)
{
	static const uint8_t addr[ETH_ALEN] = { 0xff, 0xff, 0xff,
						0xff, 0xff, 0xff };
	return addr;
}

#endif
