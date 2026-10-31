#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/net/ip.h>
#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/route.h>
#include <brk/net/skbuff.h>
#include <brk/net/socket.h>
#include <brk/net/udp.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/poll.h>
#include <uapi/socket.h>

#define UDP_EPHEMERAL_START 49152
#define UDP_EPHEMERAL_END 65535
#define UDP_RCV_QUEUE_MAX 64

static LIST_DEFINE(udp_socks);
static SPINLOCK_DEFINE(udp_lock);
static u16 udp_ephemeral = UDP_EPHEMERAL_START;

static bool udp_port_in_use_locked(u32 laddr, u16 lport)
{
	struct sock *sk;

	list_for_each_entry(sk, &udp_socks, node) {
		if (sk->sport != lport)
			continue;
		if (!laddr || !sk->saddr || sk->saddr == laddr)
			return true;
	}
	return false;
}

static u16 udp_alloc_port_locked(u32 laddr)
{
	for (unsigned int i = 0; i <= UDP_EPHEMERAL_END - UDP_EPHEMERAL_START;
	     i++) {
		u16 port = udp_ephemeral++;

		if (udp_ephemeral < UDP_EPHEMERAL_START)
			udp_ephemeral = UDP_EPHEMERAL_START;
		if (!udp_port_in_use_locked(laddr, port))
			return port;
	}
	return 0;
}

static int udp_autobind(struct sock *sk)
{
	irqstate_t flags;
	u16 port;

	flags = spin_lock_irqsave(&udp_lock);
	if (!sk->bound) {
		port = udp_alloc_port_locked(sk->saddr);
		if (!port) {
			spin_unlock_irqrestore(&udp_lock, flags);
			return -EADDRINUSE;
		}
		sk->sport = port;
		sk->bound = true;
		list_add_tail(&sk->node, &udp_socks);
	}
	spin_unlock_irqrestore(&udp_lock, flags);
	return 0;
}

static int udp_bind(struct socket *sock, const struct sockaddr *addr,
		    int addrlen)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
	struct sock *sk = sock->sk;
	irqstate_t flags;
	u32 laddr;
	u16 lport;

	if (!sk || addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
		return -EINVAL;

	laddr = sin->sin_addr.s_addr;
	lport = ntohs(sin->sin_port);

	flags = spin_lock_irqsave(&udp_lock);
	if (sk->bound) {
		spin_unlock_irqrestore(&udp_lock, flags);
		return -EINVAL;
	}
	if (!lport) {
		lport = udp_alloc_port_locked(laddr);
		if (!lport) {
			spin_unlock_irqrestore(&udp_lock, flags);
			return -EADDRINUSE;
		}
	} else if (udp_port_in_use_locked(laddr, lport)) {
		spin_unlock_irqrestore(&udp_lock, flags);
		return -EADDRINUSE;
	}

	sk->saddr = laddr;
	sk->sport = lport;
	sk->bound = true;
	list_add_tail(&sk->node, &udp_socks);
	spin_unlock_irqrestore(&udp_lock, flags);
	return 0;
}

static int udp_connect(struct socket *sock, const struct sockaddr *addr,
		       int addrlen)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
	struct sock *sk = sock->sk;
	irqstate_t flags;
	int err;

	if (!sk || addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
		return -EINVAL;

	flags = spin_lock_irqsave(&udp_lock);
	sk->daddr = sin->sin_addr.s_addr;
	sk->dport = ntohs(sin->sin_port);
	sk->connected = true;
	spin_unlock_irqrestore(&udp_lock, flags);

	if (!sk->bound) {
		err = udp_autobind(sk);
		if (err)
			return err;
	}
	return 0;
}

static int udp_sendmsg(struct socket *sock, const void *buf, size_t len,
		       const struct sockaddr *addr, int addrlen)
{
	struct sock *sk = sock->sk;
	struct net_device *dev;
	struct sk_buff *skb;
	struct udphdr *uh;
	u32 daddr, saddr, nexthop;
	u16 dport;
	size_t ulen;
	u32 sum;
	u8 pseudo[4];
	int err;

	if (!sk)
		return -EINVAL;

	if (addr) {
		const struct sockaddr_in *sin =
			(const struct sockaddr_in *)addr;

		if (addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
			return -EINVAL;
		daddr = sin->sin_addr.s_addr;
		dport = ntohs(sin->sin_port);
	} else {
		if (!sk->connected)
			return -EDESTADDRREQ;
		daddr = sk->daddr;
		dport = sk->dport;
	}

	if (!sk->bound) {
		err = udp_autobind(sk);
		if (err)
			return err;
	}

	err = ip_route_output(daddr, &nexthop, &dev);
	if (err)
		return err;

	/* No IP fragmentation yet: reject datagrams larger than the MTU. */
	if (dev->mtu <= sizeof(struct iphdr) + sizeof(*uh) ||
	    len > dev->mtu - sizeof(struct iphdr) - sizeof(*uh))
		return -EMSGSIZE;

	saddr = sk->saddr ? sk->saddr : dev->ip_addr;
	ulen = sizeof(*uh) + len;

	skb = skb_alloc(ulen);
	if (!skb)
		return -ENOMEM;

	uh = (struct udphdr *)skb_put(skb, sizeof(*uh));
	uh->source = htons(sk->sport);
	uh->dest = htons(dport);
	uh->len = htons((u16)ulen);
	uh->check = 0;

	if (len && copy_from_user(skb_put(skb, len), buf, len)) {
		skb_free(skb);
		return -EFAULT;
	}

	/* UDP pseudo-header: src, dst, zero, protocol, UDP length. */
	pseudo[0] = 0;
	pseudo[1] = IPPROTO_UDP;
	pseudo[2] = (u8)(ulen >> 8);
	pseudo[3] = (u8)ulen;

	sum = inet_csum_add(0, &saddr, sizeof(saddr));
	sum = inet_csum_add(sum, &daddr, sizeof(daddr));
	sum = inet_csum_add(sum, pseudo, sizeof(pseudo));
	sum = inet_csum_add(sum, uh, ulen);
	uh->check = inet_csum_fold(sum);
	if (!uh->check)
		uh->check = htons(0xffff);

	err = ip_output(skb, saddr, daddr, IPPROTO_UDP, IP_TTL_DEFAULT);
	if (err)
		return err;
	return (int)len;
}

int udp_rcv(struct sk_buff *skb)
{
	struct sock *sk, *match = NULL;
	struct udphdr *uh;
	u32 saddr, daddr, ulen;
	u16 sport, dport;
	u8 pseudo[4];
	u32 sum;
	irqstate_t flags;
	bool wake = false;

	if (skb->len < sizeof(*uh))
		goto drop;

	uh = (struct udphdr *)skb->data;
	ulen = ntohs(uh->len);
	if (ulen < sizeof(*uh) || ulen > skb->len)
		goto drop;

	skb->len = ulen;
	skb->tail = skb->data + ulen;

	saddr = skb->cb.ip.saddr;
	daddr = skb->cb.ip.daddr;
	sport = ntohs(uh->source);
	dport = ntohs(uh->dest);

	if (uh->check) {
		pseudo[0] = 0;
		pseudo[1] = IPPROTO_UDP;
		pseudo[2] = (u8)(ulen >> 8);
		pseudo[3] = (u8)ulen;

		sum = inet_csum_add(0, &saddr, sizeof(saddr));
		sum = inet_csum_add(sum, &daddr, sizeof(daddr));
		sum = inet_csum_add(sum, pseudo, sizeof(pseudo));
		sum = inet_csum_add(sum, uh, ulen);
		if (inet_csum_fold(sum) != 0)
			goto drop;
	}

	skb_pull(skb, sizeof(*uh));
	skb->cb.ip.sport = sport;
	skb->cb.ip.dport = dport;

	flags = spin_lock_irqsave(&udp_lock);
	list_for_each_entry(sk, &udp_socks, node) {
		if (sk->sport != dport)
			continue;
		if (sk->saddr && sk->saddr != daddr)
			continue;
		if (sk->connected && (sk->dport != sport || sk->daddr != saddr))
			continue;
		match = sk;
		break;
	}

	if (match) {
		spin_lock(&match->lock);
		if (match->rcv_queue.qlen < UDP_RCV_QUEUE_MAX) {
			skb_queue_tail(&match->rcv_queue, skb);
			skb = NULL;
			wake = true;
		}
		spin_unlock(&match->lock);
	}
	spin_unlock_irqrestore(&udp_lock, flags);

	if (skb)
		skb_free(skb);
	if (wake)
		task_wake_all(match);

	return 0;

drop:
	skb_free(skb);
	return -EINVAL;
}

static int udp_recvmsg(struct socket *sock, void *buf, size_t len,
		       struct sockaddr *addr, int *addrlen)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	irqstate_t flags;
	size_t n;
	int err = 0;

	if (!sk)
		return -EINVAL;

	flags = spin_lock_irqsave(&sk->lock);
	while (skb_queue_empty(&sk->rcv_queue)) {
		if (sk->shutdown) {
			spin_unlock_irqrestore(&sk->lock, flags);
			return 0;
		}
		if (sock->nonblock) {
			spin_unlock_irqrestore(&sk->lock, flags);
			return -EAGAIN;
		}
		task_sleep(sk, &sk->lock);
	}
	skb = skb_dequeue(&sk->rcv_queue);
	spin_unlock_irqrestore(&sk->lock, flags);

	if (addr && addrlen) {
		struct sockaddr_in sin = { 0 };
		int naddr;

		sin.sin_family = AF_INET;
		sin.sin_port = htons(skb->cb.ip.sport);
		sin.sin_addr.s_addr = skb->cb.ip.saddr;

		naddr = min(*addrlen, (int)sizeof(sin));
		memcpy(addr, &sin, naddr);
		*addrlen = sizeof(sin);
	}

	n = min(len, (size_t)skb->len);
	if (n && copy_to_user(buf, skb->data, n))
		err = -EFAULT;

	skb_free(skb);
	return err ? err : (int)n;
}

static int udp_shutdown(struct socket *sock, int how)
{
	struct sock *sk = sock->sk;
	irqstate_t flags;

	(void)how;

	if (!sk)
		return -EINVAL;

	flags = spin_lock_irqsave(&sk->lock);
	sk->shutdown = true;
	spin_unlock_irqrestore(&sk->lock, flags);
	task_wake_all(sk);
	return 0;
}

static unsigned int udp_poll(struct socket *sock, unsigned int events)
{
	struct sock *sk = sock->sk;
	unsigned int rev = 0;
	irqstate_t flags;

	if (!sk)
		return POLLERR;

	flags = spin_lock_irqsave(&sk->lock);
	if (!skb_queue_empty(&sk->rcv_queue) || sk->eof)
		rev |= POLLIN | POLLRDNORM;
	if (sk->err)
		rev |= POLLERR;
	if (sk->shutdown)
		rev |= POLLHUP;
	spin_unlock_irqrestore(&sk->lock, flags);

	rev |= POLLOUT | POLLWRNORM;
	return rev & (events | POLLERR | POLLHUP);
}

static int udp_getname(struct socket *sock, struct sockaddr *addr, int *addrlen,
		       bool peer)
{
	struct sock *sk = sock->sk;
	struct sockaddr_in sin = { 0 };
	int n;

	if (!sk || !addr || !addrlen)
		return -EINVAL;

	sin.sin_family = AF_INET;
	if (peer) {
		sin.sin_port = htons(sk->dport);
		sin.sin_addr.s_addr = sk->daddr;
	} else {
		sin.sin_port = htons(sk->sport);
		sin.sin_addr.s_addr = sk->saddr;
	}

	n = min(*addrlen, (int)sizeof(sin));
	memcpy(addr, &sin, n);
	*addrlen = sizeof(sin);
	return 0;
}

static int udp_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	irqstate_t flags;

	if (!sk)
		return 0;

	flags = spin_lock_irqsave(&udp_lock);
	if (!list_empty(&sk->node))
		list_del_init(&sk->node);
	spin_unlock_irqrestore(&udp_lock, flags);

	skb_queue_purge(&sk->rcv_queue);
	kfree(sk);
	sock->sk = NULL;
	return 0;
}

static const struct proto_ops udp_proto_ops = {
	.release = udp_release,
	.bind = udp_bind,
	.connect = udp_connect,
	.sendmsg = udp_sendmsg,
	.recvmsg = udp_recvmsg,
	.shutdown = udp_shutdown,
	.getname = udp_getname,
	.poll = udp_poll,
};

int udp_sock_create(struct socket *sock, int protocol)
{
	struct sock *sk;

	if (protocol && protocol != IPPROTO_UDP)
		return -EPROTONOSUPPORT;

	sk = kzalloc(sizeof(*sk));
	if (!sk)
		return -ENOMEM;

	spinlock_init(&sk->lock, "udp_sock");
	list_init(&sk->node);
	skb_queue_init(&sk->rcv_queue);
	sk->family = AF_INET;
	sk->type = SOCK_DGRAM;
	sk->protocol = IPPROTO_UDP;

	sock->sk = sk;
	sock->type = SOCK_DGRAM;
	sock->ops = &udp_proto_ops;
	return 0;
}
