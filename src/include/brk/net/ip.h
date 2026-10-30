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
	u8 version_ihl;
	u8 tos;
	u16 tot_len;
	u16 id;
	u16 frag_off;
	u8 ttl;
	u8 protocol;
	u16 check;
	u32 saddr;
	u32 daddr;
} __attribute__((packed));

#define IP_VERSION(h) ((h)->version_ihl >> 4)
#define IP_IHL(h) ((h)->version_ihl & 0x0f)

int ip_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt);

/*
 * Build and transmit an IPv4 packet.  @skb->data points at the transport
 * header and is consumed.  A zero @saddr selects the output device address.
 */
int ip_output(struct sk_buff *skb, u32 saddr, u32 daddr, u8 protocol, u8 ttl);

#endif
