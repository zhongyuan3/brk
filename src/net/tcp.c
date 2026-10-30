#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/net/ip.h>
#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/route.h>
#include <brk/net/skbuff.h>
#include <brk/net/socket.h>
#include <brk/net/tcp.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/poll.h>
#include <uapi/socket.h>

#define TCP_MSS 1460
#define TCP_DEFAULT_MSS 536
#define TCP_RCV_WND 4096
#define TCP_RCV_QUEUE_MAX 64
#define TCP_MAX_RETRIES 5
#define TCP_RTO_NS 1000000000ULL
#define TCP_TIME_WAIT_NS 2000000000ULL
#define TCP_FIN_WAIT2_NS 10000000000ULL
#define TCP_SYN_RECV_NS 10000000000ULL
#define TCP_TIMER_INTERVAL_NS 100000000ULL
#define TCP_TIMER_BATCH 8
#define TCP_EPHEMERAL_START 49152
#define TCP_EPHEMERAL_END 65535

enum tcp_state {
	TCP_CLOSE,
	TCP_LISTEN,
	TCP_SYN_SENT,
	TCP_SYN_RECV,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSE_WAIT,
	TCP_LAST_ACK,
	TCP_TIME_WAIT,
};

enum tcp_tx_kind {
	TCP_TX_NONE,
	TCP_TX_SYN,
	TCP_TX_DATA,
	TCP_TX_FIN,
};

struct tcp_sock {
	struct sock sk;
	struct list_head accept_node;
	struct tcp_sock *listener;
	enum tcp_state state;

	u32 iss;
	u32 irs;
	u32 snd_una;
	u32 snd_nxt;
	u32 snd_wnd;
	u32 rcv_nxt;
	u32 rcv_wnd;
	u16 mss;

	u32 backlog;
	u32 accept_backlog;
	struct list_head accept_queue;

	/* Retransmission state, protected by tcp_lock. */
	enum tcp_tx_kind tx_kind;
	u8 *tx_data;
	u16 tx_len;
	u32 tx_seq;
	u32 tx_retries;
	u64 rto_deadline_ns;

	/* Set once the fd is gone; the timer owns the final free. */
	bool orphan;
	u64 created_ns;
	u64 time_wait_deadline_ns;
	u64 fin_wait2_deadline_ns;

	int err; /* protected by tcp_lock */
};

static LIST_DEFINE(tcp_socks);
static SPINLOCK_DEFINE(tcp_lock);
static u16 tcp_ephemeral = TCP_EPHEMERAL_START;

static const struct proto_ops tcp_proto_ops;

static struct tcp_sock *tcp_from_sock(struct socket *sock)
{
	if (!sock || !sock->sk || sock->ops != &tcp_proto_ops)
		return NULL;
	return container_of(sock->sk, struct tcp_sock, sk);
}

static inline bool tcp_seq_after(u32 a, u32 b)
{
	return (i32)(a - b) > 0;
}

static inline bool tcp_seq_geq(u32 a, u32 b)
{
	return (i32)(a - b) >= 0;
}

static u32 tcp_isn(void)
{
	static u32 counter;

	counter += 0x9e3779b9;
	return (u32)ktime_get_boot_ns() + counter;
}

static void tcp_clear_tx_locked(struct tcp_sock *tsk)
{
	kfree(tsk->tx_data);
	tsk->tx_data = NULL;
	tsk->tx_len = 0;
	tsk->tx_kind = TCP_TX_NONE;
	tsk->tx_retries = 0;
	tsk->rto_deadline_ns = 0;
}

static void tcp_set_error_locked(struct tcp_sock *tsk, int err)
{
	tsk->err = err;
	tsk->state = TCP_CLOSE;
	tcp_clear_tx_locked(tsk);

	spin_lock(&tsk->sk.lock);
	tsk->sk.err = err;
	tsk->sk.eof = true;
	spin_unlock(&tsk->sk.lock);
}

static struct tcp_sock *tcp_alloc_sock(void)
{
	struct tcp_sock *tsk;

	tsk = kzalloc(sizeof(*tsk));
	if (!tsk)
		return NULL;

	spinlock_init(&tsk->sk.lock, "tcp_sock");
	list_init(&tsk->sk.node);
	list_init(&tsk->accept_node);
	list_init(&tsk->accept_queue);
	skb_queue_init(&tsk->sk.rcv_queue);

	tsk->sk.family = AF_INET;
	tsk->sk.type = SOCK_STREAM;
	tsk->sk.protocol = IPPROTO_TCP;

	tsk->state = TCP_CLOSE;
	tsk->mss = TCP_DEFAULT_MSS;
	tsk->rcv_wnd = TCP_RCV_WND;
	return tsk;
}

static void tcp_unlink_locked(struct tcp_sock *tsk)
{
	if (!list_empty(&tsk->sk.node))
		list_del_init(&tsk->sk.node);
	list_del_init(&tsk->accept_node);
}

static struct tcp_sock *tcp_lookup_exact_locked(u32 saddr, u16 sport, u32 daddr,
						u16 dport)
{
	struct tcp_sock *tsk;

	list_for_each_entry(tsk, &tcp_socks, sk.node) {
		if (tsk->sk.sport != dport)
			continue;
		if (tsk->sk.daddr != saddr || tsk->sk.dport != sport)
			continue;
		if (tsk->sk.saddr && tsk->sk.saddr != daddr)
			continue;
		return tsk;
	}
	return NULL;
}

static struct tcp_sock *tcp_lookup_listener_locked(u32 daddr, u16 dport)
{
	struct tcp_sock *tsk;

	list_for_each_entry(tsk, &tcp_socks, sk.node) {
		if (tsk->state != TCP_LISTEN || tsk->sk.sport != dport)
			continue;
		if (tsk->sk.saddr && tsk->sk.saddr != daddr)
			continue;
		return tsk;
	}
	return NULL;
}

static bool tcp_port_in_use_locked(u32 laddr, u16 lport)
{
	struct tcp_sock *tsk;

	list_for_each_entry(tsk, &tcp_socks, sk.node) {
		if (tsk->sk.sport != lport)
			continue;
		if (!laddr || !tsk->sk.saddr || tsk->sk.saddr == laddr)
			return true;
	}
	return false;
}

static u16 tcp_alloc_port_locked(u32 laddr)
{
	for (unsigned int i = 0; i <= TCP_EPHEMERAL_END - TCP_EPHEMERAL_START;
	     i++) {
		u16 port = tcp_ephemeral++;

		if (tcp_ephemeral < TCP_EPHEMERAL_START)
			tcp_ephemeral = TCP_EPHEMERAL_START;
		if (!tcp_port_in_use_locked(laddr, port))
			return port;
	}
	return 0;
}

static int tcp_autobind(struct tcp_sock *tsk)
{
	irqstate_t flags;
	u16 port;

	flags = spin_lock_irqsave(&tcp_lock);
	if (!tsk->sk.bound) {
		port = tcp_alloc_port_locked(tsk->sk.saddr);
		if (!port) {
			spin_unlock_irqrestore(&tcp_lock, flags);
			return -EADDRINUSE;
		}
		tsk->sk.sport = port;
		tsk->sk.bound = true;
		list_add_tail(&tsk->sk.node, &tcp_socks);
	}
	spin_unlock_irqrestore(&tcp_lock, flags);
	return 0;
}

static u16 tcp_checksum(u32 saddr, u32 daddr, const struct tcphdr *th,
			size_t len)
{
	u8 pseudo[4] = {
		0,
		IPPROTO_TCP,
		(u8)(len >> 8),
		(u8)len,
	};
	u32 sum = 0;

	sum = inet_csum_add(sum, &saddr, sizeof(saddr));
	sum = inet_csum_add(sum, &daddr, sizeof(daddr));
	sum = inet_csum_add(sum, pseudo, sizeof(pseudo));
	sum = inet_csum_add(sum, th, len);
	return inet_csum_fold(sum);
}

static int tcp_build_segment(struct tcp_sock *tsk, u32 seq, u32 ack, u8 flags,
			     const void *data, size_t len, bool with_mss,
			     struct sk_buff **skb_out, u32 *saddr_out)
{
	struct net_device *dev;
	struct sk_buff *skb;
	struct tcphdr *th;
	u32 saddr, daddr, nexthop;
	size_t hlen, tlen;
	u8 pseudo[4];
	u32 sum;
	int err;

	daddr = tsk->sk.daddr;
	if (!daddr)
		return -EDESTADDRREQ;

	err = ip_route_output(daddr, &nexthop, &dev);
	if (err)
		return err;

	saddr = tsk->sk.saddr ? tsk->sk.saddr : dev->ip_addr;
	hlen = sizeof(*th) + (with_mss ? 4 : 0);
	tlen = hlen + len;

	skb = skb_alloc(tlen);
	if (!skb)
		return -ENOMEM;

	th = (struct tcphdr *)skb_put(skb, sizeof(*th));
	memset(th, 0, sizeof(*th));
	th->source = htons(tsk->sk.sport);
	th->dest = htons(tsk->sk.dport);
	th->seq = htonl(seq);
	th->ack_seq = htonl(ack);
	th->doff_reserved = (u8)((5 + (with_mss ? 1 : 0)) << 4);
	th->flags = flags;
	th->window = htons((u16)tsk->rcv_wnd);

	if (with_mss) {
		u8 *opt = skb_put(skb, 4);

		opt[0] = TCP_OPT_MSS;
		opt[1] = 4;
		opt[2] = (u8)(TCP_MSS >> 8);
		opt[3] = (u8)TCP_MSS;
	}

	if (len && data)
		memcpy(skb_put(skb, len), data, len);

	pseudo[0] = 0;
	pseudo[1] = IPPROTO_TCP;
	pseudo[2] = (u8)(tlen >> 8);
	pseudo[3] = (u8)tlen;

	sum = inet_csum_add(0, &saddr, sizeof(saddr));
	sum = inet_csum_add(sum, &daddr, sizeof(daddr));
	sum = inet_csum_add(sum, pseudo, sizeof(pseudo));
	sum = inet_csum_add(sum, th, tlen);
	th->check = inet_csum_fold(sum);
	if (!th->check)
		th->check = htons(0xffff);

	*skb_out = skb;
	*saddr_out = saddr;
	return 0;
}

static int tcp_xmit_skb(struct sk_buff *skb, u32 saddr, u32 daddr)
{
	int err;

	err = ip_output(skb, saddr, daddr, IPPROTO_TCP, IP_TTL_DEFAULT);
	/* -EAGAIN only means the frame is queued behind ARP. */
	return err == -EAGAIN ? 0 : err;
}

static int tcp_send_segment(struct tcp_sock *tsk, u32 seq, u32 ack, u8 flags,
			    const void *data, size_t len, bool with_mss)
{
	struct sk_buff *skb;
	u32 saddr;
	int err;

	err = tcp_build_segment(tsk, seq, ack, flags, data, len, with_mss, &skb,
				&saddr);
	if (err)
		return err;

	return tcp_xmit_skb(skb, saddr, tsk->sk.daddr);
}

static struct tcp_sock *tcp_create_child(struct tcp_sock *listener, u32 saddr,
					 u16 sport, u32 daddr, u16 dport,
					 u32 seq, u16 window, u16 mss)
{
	struct tcp_sock *tsk;

	tsk = tcp_alloc_sock();
	if (!tsk)
		return NULL;

	tsk->sk.saddr = listener->sk.saddr ? listener->sk.saddr : daddr;
	tsk->sk.sport = dport;
	tsk->sk.daddr = saddr;
	tsk->sk.dport = sport;
	tsk->sk.bound = true;
	tsk->sk.connected = true;

	tsk->listener = listener;
	tsk->state = TCP_SYN_RECV;
	tsk->irs = seq;
	tsk->rcv_nxt = seq + 1;
	tsk->iss = tcp_isn();
	tsk->snd_una = tsk->iss;
	tsk->snd_nxt = tsk->iss + 1;
	tsk->snd_wnd = window;
	tsk->mss = mss ? min(mss, (u16)TCP_MSS) : TCP_DEFAULT_MSS;
	tsk->created_ns = ktime_get_boot_ns();

	list_add_tail(&tsk->sk.node, &tcp_socks);
	return tsk;
}

int tcp_rcv(struct sk_buff *skb)
{
	struct tcp_sock *tsk = NULL, *listener = NULL;
	struct tcphdr *th;
	u32 saddr = skb->cb.ip.saddr;
	u32 daddr = skb->cb.ip.daddr;
	u32 seq, ack, doff, payload_len;
	u16 sport, dport, window, mss = 0;
	u8 tflags;
	irqstate_t flags;
	bool wake_child = false, wake_listener = false, need_tx = false;
	u32 tx_seq = 0, tx_ack = 0;
	u8 tx_flags = 0;
	bool tx_mss = false;

	if (skb->len < sizeof(*th))
		goto drop;

	th = (struct tcphdr *)skb->data;
	doff = (u32)(th->doff_reserved >> 4) * 4;
	if (doff < sizeof(*th) || doff > skb->len)
		goto drop;
	if (tcp_checksum(saddr, daddr, th, skb->len) != 0)
		goto drop;

	sport = ntohs(th->source);
	dport = ntohs(th->dest);
	seq = ntohl(th->seq);
	ack = ntohl(th->ack_seq);
	tflags = th->flags;
	window = ntohs(th->window);

	if (tflags & TCP_SYN) {
		u8 *opt = skb->data + sizeof(*th);
		u8 *end = skb->data + doff;

		while (opt < end) {
			u8 kind, olen;

			if (opt[0] == TCP_OPT_END)
				break;
			if (opt[0] == TCP_OPT_NOP) {
				opt++;
				continue;
			}
			if (opt + 1 >= end)
				break;
			kind = opt[0];
			olen = opt[1];
			if (olen < 2 || opt + olen > end)
				break;
			if (kind == TCP_OPT_MSS && olen == 4)
				mss = (u16)(((u16)opt[2] << 8) | opt[3]);
			opt += olen;
		}
	}

	payload_len = skb->len - doff;
	skb_pull(skb, doff);
	skb->cb.ip.sport = sport;
	skb->cb.ip.dport = dport;

	flags = spin_lock_irqsave(&tcp_lock);

	tsk = tcp_lookup_exact_locked(saddr, sport, daddr, dport);
	if (!tsk && (tflags & (TCP_SYN | TCP_ACK)) == TCP_SYN) {
		listener = tcp_lookup_listener_locked(daddr, dport);
		if (listener) {
			tsk = tcp_create_child(listener, saddr, sport, daddr,
					       dport, seq, window, mss);
			if (!tsk)
				goto unlock_drop;
			tx_seq = tsk->iss;
			tx_ack = tsk->rcv_nxt;
			tx_flags = TCP_SYN | TCP_ACK;
			tx_mss = true;
			need_tx = true;
		}
	}
	if (!tsk)
		goto unlock_drop;
	if (tsk->state == TCP_LISTEN)
		goto unlock_drop;

	if (tflags & TCP_RST) {
		int rerr = tsk->state == TCP_SYN_SENT ? -ECONNREFUSED :
							-ECONNRESET;

		tcp_set_error_locked(tsk, rerr);
		wake_child = true;
		goto out_unlock;
	}

	if (tsk->state == TCP_SYN_SENT) {
		if ((tflags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		    ack == tsk->snd_nxt) {
			tsk->irs = seq;
			tsk->rcv_nxt = seq + 1;
			tsk->snd_una = ack;
			tsk->snd_nxt = ack;
			tsk->snd_wnd = window;
			tsk->mss = mss ? min(mss, (u16)TCP_MSS) :
					 TCP_DEFAULT_MSS;
			tsk->state = TCP_ESTABLISHED;
			tsk->sk.connected = true;
			tcp_clear_tx_locked(tsk);
			wake_child = true;
			tx_seq = tsk->snd_nxt;
			tx_ack = tsk->rcv_nxt;
			tx_flags = TCP_ACK;
			need_tx = true;
		}
		goto out_unlock;
	}

	if (tsk->state == TCP_SYN_RECV) {
		if ((tflags & TCP_SYN) && !(tflags & TCP_ACK)) {
			tx_seq = tsk->iss;
			tx_ack = tsk->rcv_nxt;
			tx_flags = TCP_SYN | TCP_ACK;
			tx_mss = true;
			need_tx = true;
			goto out_unlock;
		}
		if (!(tflags & TCP_ACK) || ack != tsk->snd_nxt)
			goto out_unlock;

		tsk->snd_una = ack;
		tsk->snd_wnd = window;
		tsk->state = TCP_ESTABLISHED;
		tsk->sk.connected = true;

		if (tsk->listener && list_empty(&tsk->accept_node)) {
			listener = tsk->listener;
			list_add_tail(&tsk->accept_node,
				      &listener->accept_queue);
			listener->accept_backlog++;
			wake_listener = true;
		}
	}

	if (tflags & TCP_ACK) {
		if (tcp_seq_after(ack, tsk->snd_una) &&
		    !tcp_seq_after(ack, tsk->snd_nxt))
			tsk->snd_una = ack;

		if (tsk->tx_kind == TCP_TX_DATA &&
		    tcp_seq_geq(ack, tsk->tx_seq + tsk->tx_len)) {
			tcp_clear_tx_locked(tsk);
			wake_child = true;
		}
		if (tsk->tx_kind == TCP_TX_FIN &&
		    tcp_seq_geq(ack, tsk->tx_seq + 1)) {
			tcp_clear_tx_locked(tsk);
			wake_child = true;
		}
	}

	if (payload_len) {
		if (seq == tsk->rcv_nxt) {
			if (tsk->orphan) {
				/* No reader left: consume and acknowledge. */
				tsk->rcv_nxt += payload_len;
				tx_seq = tsk->snd_nxt;
				tx_ack = tsk->rcv_nxt;
				tx_flags = TCP_ACK;
				need_tx = true;
			} else {
				bool queued = false;

				spin_lock(&tsk->sk.lock);
				if (tsk->sk.rcv_queue.qlen <
				    TCP_RCV_QUEUE_MAX) {
					skb_queue_tail(&tsk->sk.rcv_queue, skb);
					skb = NULL;
					tsk->rcv_nxt += payload_len;
					queued = true;
					wake_child = true;
				}
				spin_unlock(&tsk->sk.lock);

				if (queued) {
					tx_seq = tsk->snd_nxt;
					tx_ack = tsk->rcv_nxt;
					tx_flags = TCP_ACK;
					need_tx = true;
				}
			}
		} else {
			tx_seq = tsk->snd_nxt;
			tx_ack = tsk->rcv_nxt;
			tx_flags = TCP_ACK;
			need_tx = true;
		}
	}

	if (tflags & TCP_FIN) {
		if (seq + payload_len == tsk->rcv_nxt) {
			tsk->rcv_nxt++;

			switch (tsk->state) {
			case TCP_ESTABLISHED:
				tsk->state = TCP_CLOSE_WAIT;
				break;
			case TCP_FIN_WAIT_1:
			case TCP_FIN_WAIT_2:
				tsk->state = TCP_TIME_WAIT;
				tsk->time_wait_deadline_ns =
					ktime_get_boot_ns() + TCP_TIME_WAIT_NS;
				break;
			default:
				break;
			}

			spin_lock(&tsk->sk.lock);
			tsk->sk.eof = true;
			spin_unlock(&tsk->sk.lock);
			wake_child = true;
			tx_seq = tsk->snd_nxt;
			tx_ack = tsk->rcv_nxt;
			tx_flags = TCP_ACK;
			need_tx = true;
		} else {
			tx_seq = tsk->snd_nxt;
			tx_ack = tsk->rcv_nxt;
			tx_flags = TCP_ACK;
			need_tx = true;
		}
	}

	if (tsk->state == TCP_FIN_WAIT_1 && (tflags & TCP_ACK) &&
	    ack == tsk->snd_nxt) {
		tsk->state = TCP_FIN_WAIT_2;
		tsk->fin_wait2_deadline_ns =
			ktime_get_boot_ns() + TCP_FIN_WAIT2_NS;
	}
	if (tsk->state == TCP_LAST_ACK && (tflags & TCP_ACK) &&
	    ack == tsk->snd_nxt)
		tsk->state = TCP_CLOSE;

out_unlock:
	spin_unlock_irqrestore(&tcp_lock, flags);
	if (skb)
		skb_free(skb);
	if (need_tx)
		tcp_send_segment(tsk, tx_seq, tx_ack, tx_flags, NULL, 0,
				 tx_mss);
	if (wake_child)
		task_wake_all(&tsk->sk);
	if (wake_listener && listener)
		task_wake_all(&listener->sk);
	return 0;

unlock_drop:
	spin_unlock_irqrestore(&tcp_lock, flags);
drop:
	skb_free(skb);
	return -EINVAL;
}

static int tcp_bind(struct socket *sock, const struct sockaddr *addr,
		    int addrlen)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
	struct tcp_sock *tsk = tcp_from_sock(sock);
	irqstate_t flags;
	u32 laddr;
	u16 lport;

	if (!tsk || addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
		return -EINVAL;

	laddr = sin->sin_addr.s_addr;
	lport = ntohs(sin->sin_port);

	flags = spin_lock_irqsave(&tcp_lock);
	if (tsk->sk.bound) {
		spin_unlock_irqrestore(&tcp_lock, flags);
		return -EINVAL;
	}
	if (!lport) {
		lport = tcp_alloc_port_locked(laddr);
		if (!lport) {
			spin_unlock_irqrestore(&tcp_lock, flags);
			return -EADDRINUSE;
		}
	} else if (tcp_port_in_use_locked(laddr, lport)) {
		spin_unlock_irqrestore(&tcp_lock, flags);
		return -EADDRINUSE;
	}

	tsk->sk.saddr = laddr;
	tsk->sk.sport = lport;
	tsk->sk.bound = true;
	list_add_tail(&tsk->sk.node, &tcp_socks);
	spin_unlock_irqrestore(&tcp_lock, flags);
	return 0;
}

static int tcp_connect(struct socket *sock, const struct sockaddr *addr,
		       int addrlen)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
	struct tcp_sock *tsk = tcp_from_sock(sock);
	irqstate_t flags;
	enum tcp_state st;
	int err;

	if (!tsk || addrlen < (int)sizeof(*sin) || sin->sin_family != AF_INET)
		return -EINVAL;

	flags = spin_lock_irqsave(&tcp_lock);
	if (tsk->state != TCP_CLOSE) {
		spin_unlock_irqrestore(&tcp_lock, flags);
		return -EISCONN;
	}
	spin_unlock_irqrestore(&tcp_lock, flags);

	tsk->sk.daddr = sin->sin_addr.s_addr;
	tsk->sk.dport = ntohs(sin->sin_port);
	tsk->sk.connected = true;

	if (!tsk->sk.bound) {
		err = tcp_autobind(tsk);
		if (err)
			return err;
	}

	flags = spin_lock_irqsave(&tcp_lock);
	tsk->iss = tcp_isn();
	tsk->snd_una = tsk->iss;
	tsk->snd_nxt = tsk->iss + 1;
	tsk->rcv_nxt = 0;
	tsk->state = TCP_SYN_SENT;
	tsk->err = 0;
	spin_lock(&tsk->sk.lock);
	tsk->sk.err = 0;
	tsk->sk.eof = false;
	tsk->sk.shutdown = false;
	spin_unlock(&tsk->sk.lock);
	tcp_clear_tx_locked(tsk);
	tsk->tx_kind = TCP_TX_SYN;
	tsk->tx_seq = tsk->iss;
	tsk->tx_len = 0;
	tsk->tx_retries = 0;
	tsk->rto_deadline_ns = ktime_get_boot_ns() + TCP_RTO_NS;
	spin_unlock_irqrestore(&tcp_lock, flags);

	err = tcp_send_segment(tsk, tsk->iss, 0, TCP_SYN, NULL, 0, true);
	if (err) {
		flags = spin_lock_irqsave(&tcp_lock);
		tcp_clear_tx_locked(tsk);
		tsk->state = TCP_CLOSE;
		spin_unlock_irqrestore(&tcp_lock, flags);
		return err;
	}

	flags = spin_lock_irqsave(&tcp_lock);
	while (tsk->state == TCP_SYN_SENT && !tsk->err)
		task_sleep(&tsk->sk, &tcp_lock);
	st = tsk->state;
	err = tsk->err;
	spin_unlock_irqrestore(&tcp_lock, flags);

	if (err)
		return err;
	return st == TCP_ESTABLISHED ? 0 : -ECONNREFUSED;
}

static int tcp_listen(struct socket *sock, int backlog)
{
	struct tcp_sock *tsk = tcp_from_sock(sock);
	irqstate_t flags;
	int err;

	if (!tsk)
		return -EINVAL;

	if (!tsk->sk.bound) {
		err = tcp_autobind(tsk);
		if (err)
			return err;
	}

	flags = spin_lock_irqsave(&tcp_lock);
	if (tsk->state != TCP_CLOSE) {
		spin_unlock_irqrestore(&tcp_lock, flags);
		return -EINVAL;
	}
	tsk->state = TCP_LISTEN;
	tsk->backlog = backlog > 0 ? (u32)backlog : 1;
	spin_unlock_irqrestore(&tcp_lock, flags);
	return 0;
}

static int tcp_accept(struct socket *sock, struct socket **newsock)
{
	struct tcp_sock *tsk = tcp_from_sock(sock);
	struct tcp_sock *child;
	struct socket *ns;
	irqstate_t flags;

	if (!tsk || tsk->state != TCP_LISTEN)
		return -EINVAL;

	flags = spin_lock_irqsave(&tcp_lock);
	while (list_empty(&tsk->accept_queue)) {
		if (tsk->err) {
			int err = tsk->err;

			spin_unlock_irqrestore(&tcp_lock, flags);
			return err;
		}
		task_sleep(&tsk->sk, &tcp_lock);
	}

	child = list_first_entry(&tsk->accept_queue, struct tcp_sock,
				 accept_node);
	list_del_init(&child->accept_node);
	if (tsk->accept_backlog)
		tsk->accept_backlog--;
	child->listener = NULL;
	spin_unlock_irqrestore(&tcp_lock, flags);

	ns = kzalloc(sizeof(*ns));
	if (!ns) {
		flags = spin_lock_irqsave(&tcp_lock);
		tcp_unlink_locked(child);
		spin_unlock_irqrestore(&tcp_lock, flags);
		skb_queue_purge(&child->sk.rcv_queue);
		kfree(child);
		return -ENOMEM;
	}

	ns->type = SOCK_STREAM;
	ns->sk = &child->sk;
	ns->ops = &tcp_proto_ops;
	*newsock = ns;
	return 0;
}

static int tcp_sendmsg(struct socket *sock, const void *buf, size_t len,
		       const struct sockaddr *addr, int addrlen)
{
	struct tcp_sock *tsk = tcp_from_sock(sock);
	size_t off = 0;

	(void)addr;
	(void)addrlen;

	if (!tsk)
		return -EINVAL;

	while (off < len) {
		size_t n = min(len - off, (size_t)tsk->mss);
		u8 *copy;
		u32 seq, ack;
		irqstate_t flags;
		int err;

		if (!n)
			break;

		copy = kmalloc(n);
		if (!copy)
			return -ENOMEM;
		if (copy_from_user(copy, (const u8 *)buf + off, n)) {
			kfree(copy);
			return -EFAULT;
		}

		flags = spin_lock_irqsave(&tcp_lock);
		if (tsk->err) {
			err = tsk->err;
			spin_unlock_irqrestore(&tcp_lock, flags);
			kfree(copy);
			return err;
		}
		if (tsk->state != TCP_ESTABLISHED &&
		    tsk->state != TCP_CLOSE_WAIT) {
			spin_unlock_irqrestore(&tcp_lock, flags);
			kfree(copy);
			return -ENOTCONN;
		}
		while (!tsk->err && tsk->tx_kind != TCP_TX_NONE)
			task_sleep(&tsk->sk, &tcp_lock);

		if (tsk->err) {
			err = tsk->err;
			spin_unlock_irqrestore(&tcp_lock, flags);
			kfree(copy);
			return err;
		}
		if (tsk->state != TCP_ESTABLISHED &&
		    tsk->state != TCP_CLOSE_WAIT) {
			spin_unlock_irqrestore(&tcp_lock, flags);
			kfree(copy);
			return -ENOTCONN;
		}

		seq = tsk->snd_nxt;
		ack = tsk->rcv_nxt;
		tsk->snd_nxt += n;
		tsk->tx_kind = TCP_TX_DATA;
		tsk->tx_data = copy;
		tsk->tx_len = (u16)n;
		tsk->tx_seq = seq;
		tsk->tx_retries = 0;
		tsk->rto_deadline_ns = ktime_get_boot_ns() + TCP_RTO_NS;
		spin_unlock_irqrestore(&tcp_lock, flags);

		err = tcp_send_segment(tsk, seq, ack, TCP_ACK | TCP_PSH, copy,
				       n, false);
		if (err) {
			flags = spin_lock_irqsave(&tcp_lock);
			if (tsk->tx_kind == TCP_TX_DATA &&
			    tsk->tx_data == copy) {
				tcp_clear_tx_locked(tsk);
				tsk->snd_nxt = seq;
			}
			spin_unlock_irqrestore(&tcp_lock, flags);
			return err;
		}

		flags = spin_lock_irqsave(&tcp_lock);
		while (!tsk->err && tsk->tx_kind == TCP_TX_DATA)
			task_sleep(&tsk->sk, &tcp_lock);
		err = tsk->err;
		spin_unlock_irqrestore(&tcp_lock, flags);

		if (err)
			return err;
		off += n;
	}

	return (int)len;
}

static int tcp_recvmsg(struct socket *sock, void *buf, size_t len,
		       struct sockaddr *addr, int *addrlen)
{
	struct tcp_sock *tsk = tcp_from_sock(sock);
	struct sock *sk;
	struct sk_buff *skb;
	irqstate_t flags;
	size_t n;
	int err = 0;

	if (!tsk)
		return -EINVAL;
	if (tsk->state == TCP_LISTEN)
		return -ENOTCONN;
	sk = &tsk->sk;

	flags = spin_lock_irqsave(&sk->lock);
	while (skb_queue_empty(&sk->rcv_queue) && !sk->eof && !sk->err) {
		if (sock->nonblock) {
			spin_unlock_irqrestore(&sk->lock, flags);
			return -EAGAIN;
		}
		task_sleep(sk, &sk->lock);
	}
	if (sk->err) {
		err = sk->err;
		spin_unlock_irqrestore(&sk->lock, flags);
		return err;
	}
	if (skb_queue_empty(&sk->rcv_queue)) {
		spin_unlock_irqrestore(&sk->lock, flags);
		return 0;
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

static int tcp_shutdown(struct socket *sock, int how)
{
	struct tcp_sock *tsk = tcp_from_sock(sock);
	irqstate_t flags;
	u32 seq, ack;
	bool send_fin = false;

	if (!tsk)
		return -EINVAL;

	if (how == SHUT_RD || how == SHUT_RDWR) {
		flags = spin_lock_irqsave(&tsk->sk.lock);
		tsk->sk.shutdown = true;
		tsk->sk.eof = true;
		spin_unlock_irqrestore(&tsk->sk.lock, flags);
		task_wake_all(&tsk->sk);
	}

	if (how != SHUT_WR && how != SHUT_RDWR)
		return 0;

	flags = spin_lock_irqsave(&tcp_lock);
	if (tsk->state == TCP_ESTABLISHED || tsk->state == TCP_CLOSE_WAIT) {
		tcp_clear_tx_locked(tsk);
		seq = tsk->snd_nxt++;
		ack = tsk->rcv_nxt;
		tsk->tx_kind = TCP_TX_FIN;
		tsk->tx_seq = seq;
		tsk->tx_len = 0;
		tsk->tx_retries = 0;
		tsk->rto_deadline_ns = ktime_get_boot_ns() + TCP_RTO_NS;
		tsk->state = tsk->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK :
							    TCP_FIN_WAIT_1;
		send_fin = true;
	}
	spin_unlock_irqrestore(&tcp_lock, flags);

	if (send_fin)
		tcp_send_segment(tsk, seq, ack, TCP_FIN | TCP_ACK, NULL, 0,
				 false);
	return 0;
}

static unsigned int tcp_poll(struct socket *sock, unsigned int events)
{
	struct tcp_sock *tsk = tcp_from_sock(sock);
	struct sock *sk;
	unsigned int rev = 0;
	irqstate_t flags;

	if (!tsk)
		return POLLERR;
	sk = &tsk->sk;

	flags = spin_lock_irqsave(&sk->lock);
	if (!skb_queue_empty(&sk->rcv_queue) || sk->eof || sk->err)
		rev |= POLLIN | POLLRDNORM;
	if (sk->err)
		rev |= POLLERR;
	if (sk->eof)
		rev |= POLLHUP;
	spin_unlock_irqrestore(&sk->lock, flags);

	flags = spin_lock_irqsave(&tcp_lock);
	if ((tsk->state == TCP_ESTABLISHED || tsk->state == TCP_CLOSE_WAIT) &&
	    tsk->snd_una == tsk->snd_nxt)
		rev |= POLLOUT | POLLWRNORM;
	if (tsk->state == TCP_LISTEN && !list_empty(&tsk->accept_queue))
		rev |= POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&tcp_lock, flags);

	return rev & (events | POLLERR | POLLHUP);
}

static int tcp_getname(struct socket *sock, struct sockaddr *addr, int *addrlen,
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

static int tcp_release(struct socket *sock)
{
	struct tcp_sock *tsk = tcp_from_sock(sock);
	struct tcp_sock *child, *tmp;
	irqstate_t flags;
	u32 seq = 0, ack = 0;
	bool send_fin = false, free_now = false;

	if (!tsk)
		return 0;

	flags = spin_lock_irqsave(&tcp_lock);

	if (tsk->state == TCP_ESTABLISHED || tsk->state == TCP_CLOSE_WAIT) {
		tcp_clear_tx_locked(tsk);
		seq = tsk->snd_nxt++;
		ack = tsk->rcv_nxt;
		tsk->tx_kind = TCP_TX_FIN;
		tsk->tx_seq = seq;
		tsk->tx_len = 0;
		tsk->tx_retries = 0;
		tsk->rto_deadline_ns = ktime_get_boot_ns() + TCP_RTO_NS;
		tsk->state = tsk->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK :
							    TCP_FIN_WAIT_1;
		tsk->orphan = true;
		send_fin = true;
	} else if (tsk->state == TCP_FIN_WAIT_1 ||
		   tsk->state == TCP_FIN_WAIT_2 || tsk->state == TCP_LAST_ACK ||
		   tsk->state == TCP_TIME_WAIT) {
		tsk->orphan = true;
		if (tsk->state == TCP_TIME_WAIT && !tsk->time_wait_deadline_ns)
			tsk->time_wait_deadline_ns =
				ktime_get_boot_ns() + TCP_TIME_WAIT_NS;
		if (tsk->state == TCP_FIN_WAIT_2 && !tsk->fin_wait2_deadline_ns)
			tsk->fin_wait2_deadline_ns =
				ktime_get_boot_ns() + TCP_FIN_WAIT2_NS;
	} else {
		/* CLOSE / SYN_SENT / SYN_RECV / LISTEN: no linger. */
		free_now = true;
		tcp_clear_tx_locked(tsk);
		tcp_unlink_locked(tsk);

		if (tsk->state == TCP_LISTEN) {
			list_for_each_entry_safe(child, tmp, &tcp_socks,
						 sk.node) {
				if (child->listener != tsk)
					continue;
				tcp_unlink_locked(child);
				tcp_clear_tx_locked(child);
				skb_queue_purge(&child->sk.rcv_queue);
				kfree(child);
			}
		}
	}
	spin_unlock_irqrestore(&tcp_lock, flags);

	if (send_fin)
		tcp_send_segment(tsk, seq, ack, TCP_FIN | TCP_ACK, NULL, 0,
				 false);

	skb_queue_purge(&tsk->sk.rcv_queue);
	if (free_now)
		kfree(tsk);

	sock->sk = NULL;
	return 0;
}

static const char *tcp_state_name(enum tcp_state state)
{
	switch (state) {
	case TCP_CLOSE:
		return "CLOSE";
	case TCP_LISTEN:
		return "LISTEN";
	case TCP_SYN_SENT:
		return "SYN_SENT";
	case TCP_SYN_RECV:
		return "SYN_RECV";
	case TCP_ESTABLISHED:
		return "ESTABLISHED";
	case TCP_FIN_WAIT_1:
		return "FIN_WAIT_1";
	case TCP_FIN_WAIT_2:
		return "FIN_WAIT_2";
	case TCP_CLOSE_WAIT:
		return "CLOSE_WAIT";
	case TCP_LAST_ACK:
		return "LAST_ACK";
	case TCP_TIME_WAIT:
		return "TIME_WAIT";
	default:
		return "?";
	}
}

int tcp_proc_show(char *buf, size_t size)
{
	struct tcp_sock *tsk;
	size_t off = 0;
	irqstate_t flags;

	if (!size)
		return 0;

	flags = spin_lock_irqsave(&tcp_lock);
	off += (size_t)snprintf(buf + off, size - off,
				"state local remote orphan tx retries\n");

	list_for_each_entry(tsk, &tcp_socks, sk.node) {
		if (off >= size)
			break;
		off += (size_t)snprintf(
			buf + off, size - off,
			"%s " NET_IP4_FMT ":%u " NET_IP4_FMT ":%u %d %d %u\n",
			tcp_state_name(tsk->state), NET_IP4_ARG(tsk->sk.saddr),
			tsk->sk.sport, NET_IP4_ARG(tsk->sk.daddr),
			tsk->sk.dport, tsk->orphan ? 1 : 0, (int)tsk->tx_kind,
			tsk->tx_retries);
	}
	spin_unlock_irqrestore(&tcp_lock, flags);

	return (int)off;
}

struct tcp_timer_action {
	struct sk_buff *skb;
	u32 saddr;
	u32 daddr;
};

void tcp_timer_tick(void)
{
	static u64 next_run;
	struct tcp_timer_action actions[TCP_TIMER_BATCH];
	unsigned int nr = 0;
	struct tcp_sock *tsk, *tmp;
	u64 now = ktime_get_boot_ns();
	irqstate_t flags;

	if (now < next_run)
		return;
	next_run = now + TCP_TIMER_INTERVAL_NS;

	flags = spin_lock_irqsave(&tcp_lock);

	list_for_each_entry_safe(tsk, tmp, &tcp_socks, sk.node) {
		/* A half-open handshake that the peer abandoned. */
		if (tsk->state == TCP_SYN_RECV && !tsk->orphan &&
		    now - tsk->created_ns >= TCP_SYN_RECV_NS) {
			tcp_unlink_locked(tsk);
			tcp_clear_tx_locked(tsk);
			skb_queue_purge(&tsk->sk.rcv_queue);
			kfree(tsk);
			continue;
		}

		if (tsk->orphan) {
			bool free_now = false;

			if (tsk->state == TCP_TIME_WAIT &&
			    now >= tsk->time_wait_deadline_ns) {
				pr_debug("tcp: TIME_WAIT expired " NET_IP4_FMT
					 ":%u\n",
					 NET_IP4_ARG(tsk->sk.saddr),
					 tsk->sk.sport);
				free_now = true;
			} else if (tsk->state == TCP_CLOSE)
				free_now = true;
			else if (tsk->state == TCP_FIN_WAIT_2 &&
				 tsk->fin_wait2_deadline_ns &&
				 now >= tsk->fin_wait2_deadline_ns)
				free_now = true;

			if (free_now) {
				tcp_unlink_locked(tsk);
				tcp_clear_tx_locked(tsk);
				skb_queue_purge(&tsk->sk.rcv_queue);
				kfree(tsk);
				continue;
			}
		}

		if (tsk->tx_kind == TCP_TX_NONE || now < tsk->rto_deadline_ns)
			continue;

		if (tsk->tx_retries >= TCP_MAX_RETRIES) {
			tcp_set_error_locked(tsk, -ETIMEDOUT);
			if (tsk->orphan) {
				tcp_unlink_locked(tsk);
				skb_queue_purge(&tsk->sk.rcv_queue);
				kfree(tsk);
			} else {
				task_wake_all(&tsk->sk);
			}
			continue;
		}

		if (nr >= TCP_TIMER_BATCH)
			continue;

		pr_debug("tcp: retransmit kind=%u retry=%u\n",
			 (unsigned int)tsk->tx_kind, tsk->tx_retries);

		{
			struct sk_buff *skb = NULL;
			u32 saddr = 0;
			u32 seq = tsk->tx_seq;
			u32 ack = tsk->rcv_nxt;
			u8 tflags;
			bool mss = false;

			if (tsk->tx_kind == TCP_TX_SYN) {
				tflags = TCP_SYN;
				ack = 0;
				mss = true;
			} else if (tsk->tx_kind == TCP_TX_DATA) {
				tflags = TCP_ACK | TCP_PSH;
			} else if (tsk->tx_kind == TCP_TX_FIN) {
				tflags = TCP_FIN | TCP_ACK;
			} else {
				tcp_clear_tx_locked(tsk);
				continue;
			}

			if (tcp_build_segment(
				    tsk, seq, ack, tflags,
				    tsk->tx_kind == TCP_TX_DATA ? tsk->tx_data :
								  NULL,
				    tsk->tx_kind == TCP_TX_DATA ? tsk->tx_len :
								  0,
				    mss, &skb, &saddr) == 0) {
				actions[nr].skb = skb;
				actions[nr].saddr = saddr;
				actions[nr].daddr = tsk->sk.daddr;
				nr++;
			}
		}

		tsk->tx_retries++;
		tsk->rto_deadline_ns =
			now + (TCP_RTO_NS << min(tsk->tx_retries, 3U));
	}

	spin_unlock_irqrestore(&tcp_lock, flags);

	for (unsigned int i = 0; i < nr; i++)
		tcp_xmit_skb(actions[i].skb, actions[i].saddr,
			     actions[i].daddr);
}

static const struct proto_ops tcp_proto_ops = {
	.release = tcp_release,
	.bind = tcp_bind,
	.connect = tcp_connect,
	.listen = tcp_listen,
	.accept = tcp_accept,
	.sendmsg = tcp_sendmsg,
	.recvmsg = tcp_recvmsg,
	.shutdown = tcp_shutdown,
	.getname = tcp_getname,
	.poll = tcp_poll,
};

int tcp_sock_create(struct socket *sock, int protocol)
{
	struct tcp_sock *tsk;

	if (protocol && protocol != IPPROTO_TCP)
		return -EPROTONOSUPPORT;

	tsk = tcp_alloc_sock();
	if (!tsk)
		return -ENOMEM;

	sock->sk = &tsk->sk;
	sock->type = SOCK_STREAM;
	sock->ops = &tcp_proto_ops;
	return 0;
}
