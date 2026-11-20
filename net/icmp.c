#include <brk/icmp.h>
#include <brk/ip.h>
#include <brk/min_max.h>
#include <brk/net.h>
#include <brk/printk.h>
#include <brk/raw.h>
#include <brk/skbuff.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

void icmp_send_error(u8 type, u8 code, u32 saddr, u32 daddr, u8 protocol,
		     const void *payload, size_t payload_len)
{
	struct sk_buff *skb;
	struct icmphdr *icmp;
	struct iphdr *orig;
	size_t copy_len = min(payload_len, sizeof(u64));
	size_t total = sizeof(*icmp) + sizeof(*orig) + copy_len;

	if (!payload || !payload_len || !saddr || !daddr ||
	    saddr == 0xffffffffU || daddr == 0xffffffffU ||
	    protocol == IPPROTO_ICMP)
		return;

	skb = skb_alloc(total);
	if (!skb)
		return;

	icmp = (struct icmphdr *)skb_put(skb, sizeof(*icmp));
	memset(icmp, 0, sizeof(*icmp));
	icmp->type = type;
	icmp->code = code;

	orig = (struct iphdr *)skb_put(skb, sizeof(*orig));
	memset(orig, 0, sizeof(*orig));
	orig->version_ihl = 0x45;
	orig->tot_len =
		htons((u16)(sizeof(*orig) +
			    (payload_len > 65515 ? 65515 : payload_len)));
	orig->protocol = protocol;
	orig->saddr = saddr;
	orig->daddr = daddr;
	orig->check = inet_csum(orig, sizeof(*orig));

	memcpy(skb_put(skb, copy_len), payload, copy_len);
	icmp->checksum = inet_csum(icmp, skb->len);

	(void)ip_output(skb, daddr, saddr, IPPROTO_ICMP, IP_TTL_DEFAULT);
}

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

	raw_deliver(IPPROTO_ICMP, skb, saddr, daddr);

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
