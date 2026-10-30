#ifndef BRK_NET_ICMP_H
#define BRK_NET_ICMP_H

#include <brk/net/skbuff.h>
#include <brk/types.h>

#define ICMP_ECHOREPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO 8

struct icmphdr {
	u8 type;
	u8 code;
	u16 checksum;
	u16 id;
	u16 sequence;
} __attribute__((packed));

int icmp_rcv(struct sk_buff *skb);

int icmp_send_echo(u32 saddr, u32 daddr, u16 id, u16 seq, const void *data,
		   size_t len);

#endif
