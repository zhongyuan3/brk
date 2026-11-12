#ifndef BRK_ICMP_H
#define BRK_ICMP_H

#include <brk/compiler_attr.h>
#include <brk/skbuff.h>
#include <brk/types.h>
#include <stddef.h>

#define ICMP_ECHOREPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO 8
#define ICMP_TIME_EXCEEDED 11

#define ICMP_NET_UNREACH 0
#define ICMP_HOST_UNREACH 1
#define ICMP_PROT_UNREACH 2
#define ICMP_PORT_UNREACH 3
#define ICMP_FRAG_NEEDED 4

struct icmphdr {
	u8 type;
	u8 code;
	u16 checksum;
	u16 id;
	u16 sequence;
} __packed;

int icmp_rcv(struct sk_buff *skb);

void icmp_send_error(u8 type, u8 code, u32 saddr, u32 daddr, u8 protocol,
		     const void *payload, size_t payload_len);

#endif
