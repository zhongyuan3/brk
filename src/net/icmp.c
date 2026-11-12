#include <brk/icmp.h>
#include <brk/ip.h>
#include <brk/net.h>
#include <brk/printk.h>
#include <brk/skbuff.h>
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
