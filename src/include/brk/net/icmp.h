#ifndef BRK_NET_ICMP_H
#define BRK_NET_ICMP_H

#include <brk/net/skbuff.h>
#include <brk/types.h>

#define ICMP_ECHOREPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO 8

struct icmphdr {
	uint8_t type;
	uint8_t code;
	uint16_t checksum;
	uint16_t id;
	uint16_t sequence;
} __attribute__((packed));

int icmp_rcv(struct sk_buff *skb);

int icmp_send_echo(uint32_t saddr, uint32_t daddr, uint16_t id, uint16_t seq,
		   const void *data, size_t len);

#endif
