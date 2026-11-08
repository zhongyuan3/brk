#ifndef BRK_UDP_H
#define BRK_UDP_H

#include <brk/compiler_attr.h>
#include <brk/skbuff.h>
#include <brk/types.h>

struct socket;

struct udphdr {
	u16 source;
	u16 dest;
	u16 len;
	u16 check;
} __packed;

int udp_rcv(struct sk_buff *skb);
int udp_sock_create(struct socket *sock, int protocol);

#endif
