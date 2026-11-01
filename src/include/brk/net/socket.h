#ifndef BRK_NET_SOCKET_H
#define BRK_NET_SOCKET_H

#include <brk/list.h>
#include <brk/net/skbuff.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <uapi/socket.h>

struct fs_file_ops;
struct socket;

/*
 * Protocol-private state.  Field updates after bind/connect are stable, so
 * the receive path can read them while holding udp_lock.
 */
struct sock {
	spinlock_t lock;
	uint16_t family;
	uint16_t type;
	uint16_t protocol;

	uint32_t saddr;
	uint32_t daddr;
	uint16_t sport;
	uint16_t dport;

	bool bound;
	bool connected;
	bool shutdown;
	bool eof;
	int err;

	struct sk_buff_head rcv_queue;
	struct list_head node;
};

struct proto_ops {
	int (*release)(struct socket *sock);
	int (*bind)(struct socket *sock, const struct sockaddr *addr,
		    int addrlen);
	int (*connect)(struct socket *sock, const struct sockaddr *addr,
		       int addrlen);
	int (*listen)(struct socket *sock, int backlog);
	int (*accept)(struct socket *sock, struct socket **newsock);
	int (*sendmsg)(struct socket *sock, const void *buf, size_t len,
		       const struct sockaddr *addr, int addrlen);
	int (*recvmsg)(struct socket *sock, void *buf, size_t len,
		       struct sockaddr *addr, int *addrlen);
	int (*shutdown)(struct socket *sock, int how);
	int (*getname)(struct socket *sock, struct sockaddr *addr, int *addrlen,
		       bool peer);
	unsigned int (*poll)(struct socket *sock, unsigned int events);
};

struct socket {
	int type;
	bool nonblock;
	struct sock *sk;
	const struct proto_ops *ops;
};

extern const struct fs_file_ops socket_file_ops;

int sock_create(int family, int type, int protocol, struct socket **res);
void sock_release(struct socket *sock);

#endif
