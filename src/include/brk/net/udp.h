#ifndef BRK_NET_UDP_H
#define BRK_NET_UDP_H

#include <brk/net/skbuff.h>
#include <brk/types.h>

struct socket;

struct udphdr {
	u16 source;
	u16 dest;
	u16 len;
	u16 check;
} __attribute__((packed));

int udp_rcv(struct sk_buff *skb);
int udp_sock_create(struct socket *sock, int protocol);

#endif
