#ifndef BRK_NET_ARP_H
#define BRK_NET_ARP_H

#include <brk/net/netdevice.h>
#include <brk/net/skbuff.h>
#include <brk/types.h>

#define ARP_HWTYPE_ETHER 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

struct arphdr {
	u16 ar_hrd;
	u16 ar_pro;
	u8 ar_hln;
	u8 ar_pln;
	u16 ar_op;
	u8 ar_sha[6];
	u32 ar_spa;
	u8 ar_tha[6];
	u32 ar_tpa;
} __attribute__((packed));

void arp_init(void);

int arp_rcv(struct sk_buff *skb, struct net_device *dev,
	    struct packet_type *pt);

/*
 * Transmit an IPv4 packet to @next_hop, resolving its link address first.
 * Consumes @skb in every case.  Returns 0 when the frame was handed to the
 * device, or -EAGAIN when the packet was queued behind an ARP request.
 */
int arp_output(struct sk_buff *skb, struct net_device *dev, u32 next_hop);

#endif
