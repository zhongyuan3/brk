#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/net/eth.h>
#include <brk/net/netdevice.h>
#include <brk/net/skbuff.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>

static LIST_DEFINE(netdev_list);
static SPINLOCK_DEFINE(netdev_lock);
static LIST_DEFINE(ptype_list);
static SPINLOCK_DEFINE(ptype_lock);
static unsigned int netdev_ifindex;

static struct net_device *netdev_lookup_locked(const char *name)
{
	struct net_device *dev;

	list_for_each_entry(dev, &netdev_list, dev_list) {
		if (!strcmp(dev->name, name))
			return dev;
	}
	return NULL;
}

struct net_device *netdev_alloc(const char *name)
{
	struct net_device *dev;

	if (!name || !name[0])
		return NULL;

	dev = kzalloc(sizeof(*dev));
	if (!dev)
		return NULL;

	strlcpy(dev->name, name, sizeof(dev->name));
	list_init(&dev->dev_list);
	spinlock_init(&dev->tx_lock, "netdev_tx");
	return dev;
}

void netdev_free(struct net_device *dev)
{
	kfree(dev);
}

int netdev_register(struct net_device *dev)
{
	irqstate_t flags;

	if (!dev || !dev->name[0])
		return -EINVAL;

	flags = spin_lock_irqsave(&netdev_lock);
	if (netdev_lookup_locked(dev->name)) {
		spin_unlock_irqrestore(&netdev_lock, flags);
		return -EEXIST;
	}
	dev->ifindex = ++netdev_ifindex;
	list_add_tail(&dev->dev_list, &netdev_list);
	spin_unlock_irqrestore(&netdev_lock, flags);

	if (dev->netdev_ops && dev->netdev_ops->ndo_open)
		dev->netdev_ops->ndo_open(dev);

	dev->flags |= IFF_UP;
	pr_info("net: registered %s\n", dev->name);
	return 0;
}

void netdev_unregister(struct net_device *dev)
{
	irqstate_t flags;

	if (!dev)
		return;

	if (dev->netdev_ops && dev->netdev_ops->ndo_stop)
		dev->netdev_ops->ndo_stop(dev);

	dev->flags &= ~IFF_UP;

	flags = spin_lock_irqsave(&netdev_lock);
	if (!list_empty(&dev->dev_list))
		list_del_init(&dev->dev_list);
	spin_unlock_irqrestore(&netdev_lock, flags);
}

struct net_device *netdev_get_by_name(const char *name)
{
	struct net_device *dev;
	irqstate_t flags;

	if (!name)
		return NULL;

	flags = spin_lock_irqsave(&netdev_lock);
	dev = netdev_lookup_locked(name);
	spin_unlock_irqrestore(&netdev_lock, flags);
	return dev;
}

void dev_add_pack(struct packet_type *pt)
{
	irqstate_t flags;

	if (!pt || !pt->func)
		return;

	flags = spin_lock_irqsave(&ptype_lock);
	list_add_tail(&pt->list, &ptype_list);
	spin_unlock_irqrestore(&ptype_lock, flags);
}

void dev_remove_pack(struct packet_type *pt)
{
	irqstate_t flags;

	if (!pt)
		return;

	flags = spin_lock_irqsave(&ptype_lock);
	if (!list_empty(&pt->list))
		list_del_init(&pt->list);
	spin_unlock_irqrestore(&ptype_lock, flags);
}

int netif_rx(struct sk_buff *skb)
{
	struct packet_type *pt;
	struct packet_type *match = NULL;
	struct net_device *dev;
	irqstate_t flags;
	int err;

	if (!skb || !skb->dev) {
		skb_free(skb);
		return -EINVAL;
	}

	dev = skb->dev;

	flags = spin_lock_irqsave(&ptype_lock);
	list_for_each_entry(pt, &ptype_list, list) {
		if (pt->type != skb->protocol && pt->type != ETH_P_ALL)
			continue;
		if (pt->dev && pt->dev != dev)
			continue;
		match = pt;
		break;
	}
	spin_unlock_irqrestore(&ptype_lock, flags);

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += skb->len;

	if (!match) {
		dev->stats.rx_dropped++;
		skb_free(skb);
		return -ENOENT;
	}

	err = match->func(skb, dev, match);
	if (err)
		dev->stats.rx_dropped++;
	return err;
}

int netdev_start_xmit(struct sk_buff *skb)
{
	struct net_device *dev;
	int err;

	if (!skb) {
		return -EINVAL;
	}

	dev = skb->dev;
	if (!dev) {
		skb_free(skb);
		return -EINVAL;
	}

	if (!(dev->flags & IFF_UP)) {
		dev->stats.tx_dropped++;
		skb_free(skb);
		return -ENETDOWN;
	}

	if (!dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) {
		dev->stats.tx_dropped++;
		skb_free(skb);
		return -ENODEV;
	}

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	/* ndo_start_xmit consumes the skb, including on error. */
	err = dev->netdev_ops->ndo_start_xmit(skb, dev);
	if (err) {
		dev->stats.tx_errors++;
		dev->stats.tx_dropped++;
	}
	return err;
}
