#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/route.h>
#include <brk/net/skbuff.h>
#include <brk/types.h>
#include <uapi/errno.h>

static int loopback_xmit(struct sk_buff *skb, struct net_device *dev)
{
	(void)dev;
	return netif_rx(skb);
}

static const struct net_device_ops loopback_ops = {
	.ndo_start_xmit = loopback_xmit,
};

int loopback_init(void)
{
	struct net_device *dev;
	int err;

	dev = netdev_alloc("lo");
	if (!dev)
		return -ENOMEM;

	dev->type = ARPHRD_LOOPBACK;
	dev->mtu = 65535;
	dev->flags = IFF_UP | IFF_LOOPBACK;
	dev->netdev_ops = &loopback_ops;
	dev->ip_addr = NET_IP4(127, 0, 0, 1);
	dev->netmask = NET_IP4(255, 0, 0, 0);

	err = netdev_register(dev);
	if (err) {
		netdev_free(dev);
		return err;
	}

	return route_add(dev->ip_addr & dev->netmask, dev->netmask, 0, dev);
}
