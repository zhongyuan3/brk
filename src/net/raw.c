#include <brk/container_of.h>
#include <brk/ip.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/min_max.h>
#include <brk/permission.h>
#include <brk/raw.h>
#include <brk/skbuff.h>
#include <brk/socket.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/capability.h>
#include <uapi/errno.h>
#include <uapi/poll.h>
#include <uapi/socket.h>

#define RAW_RCV_QUEUE_MAX 64

struct raw_sock {
	struct sock sk;
	struct list_head node;
};

static LIST_DEFINE(raw_socks);
static SPINLOCK_DEFINE(raw_lock);

static struct raw_sock *raw_from_sock(struct socket *sock)
{
	if (!sock || !sock->sk)
		return NULL;
	return container_of(sock->sk, struct raw_sock, sk);
}

static int raw_sendmsg(struct socket *sock, const void *buf, size_t len,
		       const struct sockaddr *addr, int addrlen, int flags)
{
	struct raw_sock *raw = raw_from_sock(sock);
	struct sk_buff *skb;
	u32 daddr;
	int err;

	(void)flags;

	if (!raw)
		return -EINVAL;
	if (addr) {
		const struct sockaddr_in *sin =
			(const struct sockaddr_in *)addr;

		if (addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
			return -EINVAL;
		daddr = sin->sin_addr.s_addr;
	} else if (raw->sk.connected) {
		daddr = raw->sk.daddr;
	} else {
		return -EDESTADDRREQ;
	}

	skb = skb_alloc(len);
	if (!skb)
		return -ENOMEM;
	if (len && copy_from_user(skb_put(skb, len), buf, len)) {
		skb_free(skb);
		return -EFAULT;
	}

	err = ip_output(skb, raw->sk.saddr, daddr, raw->sk.protocol,
			IP_TTL_DEFAULT);
	if (err && err != -EAGAIN)
		return err;
	return (int)len;
}

static int raw_recvmsg(struct socket *sock, void *buf, size_t len,
		       struct sockaddr *addr, int *addrlen, int msg_flags)
{
	struct raw_sock *raw = raw_from_sock(sock);
	struct sk_buff *skb;
	irqstate_t flags;
	bool nonblock;
	bool peek;
	size_t datagram_len;
	size_t n;
	int err = 0;

	if (!raw)
		return -EINVAL;
	nonblock = sock->nonblock || (msg_flags & MSG_DONTWAIT);
	peek = (msg_flags & MSG_PEEK) != 0;

	flags = spin_lock_irqsave(&raw->sk.lock);
	while (skb_queue_empty(&raw->sk.rcv_queue)) {
		if (raw->sk.shutdown || raw->sk.eof) {
			spin_unlock_irqrestore(&raw->sk.lock, flags);
			return 0;
		}
		if (nonblock) {
			spin_unlock_irqrestore(&raw->sk.lock, flags);
			return -EAGAIN;
		}
		task_sleep(&raw->sk, &raw->sk.lock);
	}
	skb = peek ? raw->sk.rcv_queue.head : skb_dequeue(&raw->sk.rcv_queue);
	spin_unlock_irqrestore(&raw->sk.lock, flags);

	if (addr && addrlen) {
		struct sockaddr_in sin = { 0 };
		int naddr;

		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = skb->cb.ip.saddr;
		naddr = min(*addrlen, (int)sizeof(sin));
		memcpy(addr, &sin, naddr);
		*addrlen = sizeof(sin);
	}

	datagram_len = skb->len;
	n = min(len, datagram_len);
	if (n && copy_to_user(buf, skb->data, n))
		err = -EFAULT;
	if (!peek)
		skb_free(skb);
	if (err)
		return err;
	return (msg_flags & MSG_TRUNC) ? (int)datagram_len : (int)n;
}

static int raw_bind(struct socket *sock, const struct sockaddr *addr,
		    int addrlen)
{
	struct raw_sock *raw = raw_from_sock(sock);
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

	if (!raw || addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
		return -EINVAL;
	raw->sk.saddr = sin->sin_addr.s_addr;
	raw->sk.bound = true;
	return 0;
}

static int raw_connect(struct socket *sock, const struct sockaddr *addr,
		       int addrlen)
{
	struct raw_sock *raw = raw_from_sock(sock);
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

	if (!raw || addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
		return -EINVAL;
	raw->sk.daddr = sin->sin_addr.s_addr;
	raw->sk.connected = true;
	return 0;
}

static int raw_shutdown(struct socket *sock, int how)
{
	struct raw_sock *raw = raw_from_sock(sock);
	irqstate_t flags;

	(void)how;
	if (!raw)
		return -EINVAL;
	flags = spin_lock_irqsave(&raw->sk.lock);
	raw->sk.shutdown = true;
	raw->sk.eof = true;
	spin_unlock_irqrestore(&raw->sk.lock, flags);
	task_wake_all(&raw->sk);
	return 0;
}

static int raw_getname(struct socket *sock, struct sockaddr *addr, int *addrlen,
		       bool peer)
{
	struct raw_sock *raw = raw_from_sock(sock);
	struct sockaddr_in sin = { 0 };
	int n;

	if (!raw || !addr || !addrlen)
		return -EINVAL;
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = peer ? raw->sk.daddr : raw->sk.saddr;
	n = min(*addrlen, (int)sizeof(sin));
	memcpy(addr, &sin, n);
	*addrlen = sizeof(sin);
	return 0;
}

static unsigned int raw_poll(struct socket *sock, unsigned int events)
{
	struct raw_sock *raw = raw_from_sock(sock);
	unsigned int rev = POLLOUT | POLLWRNORM;
	irqstate_t flags;

	if (!raw)
		return POLLERR;
	flags = spin_lock_irqsave(&raw->sk.lock);
	if (!skb_queue_empty(&raw->sk.rcv_queue) || raw->sk.eof)
		rev |= POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&raw->sk.lock, flags);
	return rev & (events | POLLERR | POLLHUP);
}

static int raw_release(struct socket *sock)
{
	struct raw_sock *raw = raw_from_sock(sock);
	irqstate_t flags;

	if (!raw)
		return 0;
	flags = spin_lock_irqsave(&raw_lock);
	if (!list_empty(&raw->node))
		list_del_init(&raw->node);
	spin_unlock_irqrestore(&raw_lock, flags);
	skb_queue_purge(&raw->sk.rcv_queue);
	kfree(raw);
	sock->sk = NULL;
	return 0;
}

static const struct proto_ops raw_proto_ops = {
	.release = raw_release,
	.bind = raw_bind,
	.connect = raw_connect,
	.sendmsg = raw_sendmsg,
	.recvmsg = raw_recvmsg,
	.shutdown = raw_shutdown,
	.getname = raw_getname,
	.poll = raw_poll,
};

int raw_sock_create(struct socket *sock, int protocol)
{
	struct raw_sock *raw;

	if (protocol != IPPROTO_ICMP)
		return -EPROTONOSUPPORT;
	if (!capable(CAP_NET_RAW))
		return -EPERM;

	raw = kzalloc(sizeof(*raw));
	if (!raw)
		return -ENOMEM;
	spinlock_init(&raw->sk.lock, "raw_sock");
	list_init(&raw->node);
	skb_queue_init(&raw->sk.rcv_queue);
	raw->sk.family = AF_INET;
	raw->sk.type = SOCK_RAW;
	raw->sk.protocol = (u16)protocol;
	raw->sk.rcvbuf = SOCK_DEFAULT_RCVBUF;
	raw->sk.sndbuf = SOCK_DEFAULT_SNDBUF;

	sock->sk = &raw->sk;
	sock->type = SOCK_RAW;
	sock->ops = &raw_proto_ops;

	spin_lock(&raw_lock);
	list_add_tail(&raw->node, &raw_socks);
	spin_unlock(&raw_lock);
	return 0;
}

void raw_deliver(u8 protocol, struct sk_buff *skb, u32 saddr, u32 daddr)
{
	struct raw_sock *raw;
	irqstate_t flags;

	flags = spin_lock_irqsave(&raw_lock);
	list_for_each_entry(raw, &raw_socks, node) {
		struct sk_buff *copy;

		if (raw->sk.protocol != protocol)
			continue;
		if (raw->sk.saddr && raw->sk.saddr != daddr)
			continue;
		if (raw->sk.connected && raw->sk.daddr != saddr)
			continue;
		if (raw->sk.rcv_queue.qlen >= RAW_RCV_QUEUE_MAX)
			continue;

		copy = skb_alloc(skb->len);
		if (!copy)
			continue;
		memcpy(skb_put(copy, skb->len), skb->data, skb->len);
		copy->dev = skb->dev;
		copy->cb = skb->cb;

		spin_lock(&raw->sk.lock);
		skb_queue_tail(&raw->sk.rcv_queue, copy);
		spin_unlock(&raw->sk.lock);
		task_wake_all(&raw->sk);
	}
	spin_unlock_irqrestore(&raw_lock, flags);
}
