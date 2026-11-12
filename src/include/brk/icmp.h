#ifndef BRK_ICMP_H
#define BRK_ICMP_H

#include <brk/compiler_attr.h>
#include <brk/skbuff.h>
#include <brk/types.h>
#include <stddef.h>

#define ICMP_ECHOREPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO 8

struct icmphdr {
	u8 type;
	u8 code;
	u16 checksum;
	u16 id;
	u16 sequence;
} __packed;

int icmp_rcv(struct sk_buff *skb);

#endif
