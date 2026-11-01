#ifndef BRK_NET_TCP_H
#define BRK_NET_TCP_H

#include <brk/net/skbuff.h>
#include <brk/types.h>

struct socket;

/* TCP flags. */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* TCP options. */
#define TCP_OPT_END 0
#define TCP_OPT_NOP 1
#define TCP_OPT_MSS 2

struct tcphdr {
	uint16_t source;
	uint16_t dest;
	uint32_t seq;
	uint32_t ack_seq;
	uint8_t doff_reserved;
	uint8_t flags;
	uint16_t window;
	uint16_t check;
	uint16_t urg_ptr;
} __attribute__((packed));

int tcp_rcv(struct sk_buff *skb);
int tcp_sock_create(struct socket *sock, int protocol);
void tcp_timer_tick(void);
int tcp_proc_show(char *buf, size_t size);

#endif
