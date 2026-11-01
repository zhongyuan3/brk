#ifndef BRK_NET_IP_H
#define BRK_NET_IP_H

#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/skbuff.h>
#include <brk/types.h>

#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFSET 0x1fff
#define IP_TTL_DEFAULT 64

struct iphdr {
	uint8_t version_ihl;
	uint8_t tos;
	uint16_t tot_len;
	uint16_t id;
	uint16_t frag_off;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t check;
	uint32_t saddr;
	uint32_t daddr;
} __attribute__((packed));

#define IP_VERSION(h) ((h)->version_ihl >> 4)
#define IP_IHL(h) ((h)->version_ihl & 0x0f)

int ip_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt);

/*
 * Build and transmit an IPv4 packet.  @skb->data points at the transport
 * header and is consumed.  A zero @saddr selects the output device address.
 */
int ip_output(struct sk_buff *skb, uint32_t saddr, uint32_t daddr,
	      uint8_t protocol, uint8_t ttl);

#endif
