#ifndef BRK_RAW_H
#define BRK_RAW_H

#include <brk/skbuff.h>
#include <brk/types.h>

struct socket;

int raw_sock_create(struct socket *sock, int protocol);
void raw_deliver(u8 protocol, struct sk_buff *skb, u32 saddr, u32 daddr);

#endif
