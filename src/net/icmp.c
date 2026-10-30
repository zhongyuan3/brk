#include <brk/net/icmp.h>
#include <brk/net/ip.h>
#include <brk/net/net.h>
#include <brk/net/skbuff.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

int icmp_rcv(struct sk_buff *skb)
{
	struct icmphdr *icmp;
	u32 saddr, daddr;
	u16 id, seq;

	if (skb->len < sizeof(*icmp)) {
		skb_free(skb);
		return -EINVAL;
	}

	icmp = (struct icmphdr *)skb->data;
	if (inet_csum(icmp, skb->len) != 0) {
		skb_free(skb);
		return -EINVAL;
	}

	saddr = skb->cb.ip.saddr;
	daddr = skb->cb.ip.daddr;
	id = ntohs(icmp->id);
	seq = ntohs(icmp->sequence);

	switch (icmp->type) {
	case ICMP_ECHO:
		if (icmp->code != 0)
			break;
		icmp->type = ICMP_ECHOREPLY;
		icmp->checksum = 0;
		icmp->checksum = inet_csum(icmp, skb->len);
		return ip_output(skb, daddr, saddr, IPPROTO_ICMP,
				 IP_TTL_DEFAULT);

	case ICMP_ECHOREPLY:
		pr_info("icmp: echo reply from " NET_IP4_FMT " id=%u seq=%u\n",
			NET_IP4_ARG(saddr), id, seq);
		break;

	default:
		break;
	}

	skb_free(skb);
	return 0;
}

int icmp_send_echo(u32 saddr, u32 daddr, u16 id, u16 seq, const void *data,
		   size_t len)
{
	struct sk_buff *skb;
	struct icmphdr *icmp;

	skb = skb_alloc(sizeof(*icmp) + len);
	if (!skb)
		return -ENOMEM;

	icmp = (struct icmphdr *)skb_put(skb, sizeof(*icmp));
	icmp->type = ICMP_ECHO;
	icmp->code = 0;
	icmp->checksum = 0;
	icmp->id = htons(id);
	icmp->sequence = htons(seq);

	if (len)
		memcpy(skb_put(skb, len), data, len);

	icmp->checksum = inet_csum(icmp, skb->len);
	return ip_output(skb, saddr, daddr, IPPROTO_ICMP, IP_TTL_DEFAULT);
}

int net_ping(u32 daddr)
{
	static u16 seq;
	u8 payload[16];

	memset(payload, 0x42, sizeof(payload));
	return icmp_send_echo(0, daddr, 0x4252, ++seq, payload,
			      sizeof(payload));
}
