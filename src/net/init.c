#include <brk/arp.h>
#include <brk/eth.h>
#include <brk/ip.h>
#include <brk/net.h>
#include <brk/netdevice.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/tcp.h>
#include <brk/types.h>

static struct packet_type arp_packet_type = {
	.type = ETH_P_ARP,
	.func = arp_rcv,
};

static struct packet_type ip_packet_type = {
	.type = ETH_P_IP,
	.func = ip_rcv,
};

void net_init(void)
{
	int err;

	arp_init();
	tcp_init();

	err = loopback_init();
	if (err)
		panic("net: loopback_init failed: %d\n", err);

	dev_add_pack(&arp_packet_type);
	dev_add_pack(&ip_packet_type);

	pr_info("net: IPv4 stack initialized\n");
}
