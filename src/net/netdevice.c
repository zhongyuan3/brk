#include <brk/assert.h>
#include <brk/eth.h>
#include <brk/kmalloc.h>
#include <brk/netdevice.h>
#include <brk/permission.h>
#include <brk/printk.h>
#include <brk/route.h>
#include <brk/skbuff.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/capability.h>
#include <uapi/errno.h>
#include <uapi/net.h>

#define NET_RX_QUEUE_MAX 128

static LIST_DEFINE(netdev_list);
static SPINLOCK_DEFINE(netdev_lock);
static LIST_DEFINE(ptype_list);
static SPINLOCK_DEFINE(ptype_lock);
static unsigned int netdev_ifindex;

static struct sk_buff_head net_rx_queue;
static SPINLOCK_DEFINE(net_rx_queue_lock);
static int net_rx_wait;
static bool net_rx_started;

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
	device_init(&dev->dev, name);
	list_init(&dev->dev_list);
	spinlock_init(&dev->tx_lock, "netdev_tx");
	return dev;
}

void netdev_free(struct net_device *dev)
{
	if (!dev)
		return;
	if (dev->dev.registered)
		device_unregister(&dev->dev);
	kfree(dev);
}

int netdev_register(struct net_device *dev)
{
	irqstate_t flags;
	int err;

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

	err = device_register(&dev->dev);
	if (err) {
		flags = spin_lock_irqsave(&netdev_lock);
		list_del_init(&dev->dev_list);
		spin_unlock_irqrestore(&netdev_lock, flags);
		return err;
	}

	pr_info("net: registered %s\n", dev->name);
	return 0;
}

void netdev_unregister(struct net_device *dev)
{
	irqstate_t flags;

	if (!dev)
		return;

	if ((dev->flags & IFF_UP) && dev->netdev_ops &&
	    dev->netdev_ops->ndo_stop)
		dev->netdev_ops->ndo_stop(dev);

	dev->flags &= ~IFF_UP;

	flags = spin_lock_irqsave(&netdev_lock);
	if (!list_empty(&dev->dev_list))
		list_del_init(&dev->dev_list);
	spin_unlock_irqrestore(&netdev_lock, flags);

	device_unregister(&dev->dev);
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

struct net_device *netdev_get_broadcast_dev(void)
{
	struct net_device *dev;
	struct net_device *found = NULL;
	irqstate_t flags;

	flags = spin_lock_irqsave(&netdev_lock);
	list_for_each_entry(dev, &netdev_list, dev_list) {
		if ((dev->flags & IFF_UP) && !(dev->flags & IFF_LOOPBACK)) {
			found = dev;
			break;
		}
	}
	spin_unlock_irqrestore(&netdev_lock, flags);
	return found;
}

int netdev_set_up(struct net_device *dev, bool up)
{
	int err;

	if (!dev)
		return -EINVAL;
	if (up == !!(dev->flags & IFF_UP))
		return 0;

	if (up) {
		if (dev->netdev_ops && dev->netdev_ops->ndo_open) {
			err = dev->netdev_ops->ndo_open(dev);
			if (err)
				return err;
		}
		dev->flags |= IFF_UP;
	} else {
		if (dev->netdev_ops && dev->netdev_ops->ndo_stop)
			dev->netdev_ops->ndo_stop(dev);
		dev->flags &= ~IFF_UP;
	}
	return 0;
}

int netdev_set_ipv4(struct net_device *dev, u32 addr, u32 mask, u32 broadcast)
{
	if (!dev)
		return -EINVAL;

	dev->ip_addr = addr;
	dev->netmask = mask;
	if (!broadcast && addr && mask)
		broadcast = addr | ~mask;
	dev->broadcast = broadcast;
	return 0;
}

static int ifreq_get_addr(const struct ifreq *ifr, u32 *addr)
{
	const struct sockaddr_in *sin =
		(const struct sockaddr_in *)&ifr->ifr_addr;

	if (sin->sin_family != AF_INET)
		return -EINVAL;
	*addr = sin->sin_addr.s_addr;
	return 0;
}

static void ifreq_set_addr(struct ifreq *ifr, u32 addr)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;

	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
}

static int sockaddr_to_ipv4(const struct sockaddr *sa, u32 *addr)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

	if (sin->sin_family != AF_INET)
		return -EINVAL;
	*addr = sin->sin_addr.s_addr;
	return 0;
}

int netdev_ioctl(unsigned int cmd, unsigned long arg)
{
	struct ifreq ifr;
	struct rtentry rt;
	struct net_device *dev;
	u32 addr, mask, gw;
	bool set_cmd;
	bool copy_ifr = false;
	int err;

	if (!arg)
		return -EINVAL;

	set_cmd = cmd == SIOCSIFFLAGS || cmd == SIOCSIFADDR ||
		  cmd == SIOCSIFNETMASK || cmd == SIOCSIFBRDADDR ||
		  cmd == SIOCSIFMTU || cmd == SIOCADDRT || cmd == SIOCDELRT;
	if (set_cmd && !capable(CAP_NET_ADMIN))
		return -EPERM;

	if (cmd == SIOCGIFCONF) {
		struct ifconf ifc;
		struct ifreq entries[8];
		unsigned int nr = 0;
		size_t bytes;
		irqstate_t flags;

		if (copy_from_user(&ifc, (void *)arg, sizeof(ifc)))
			return -EFAULT;
		if (ifc.ifc_len < 0 || !ifc.ifc_buf)
			return -EINVAL;

		flags = spin_lock_irqsave(&netdev_lock);
		list_for_each_entry(dev, &netdev_list, dev_list) {
			if (nr >= 8)
				break;
			if ((int)((nr + 1) * sizeof(struct ifreq)) >
			    ifc.ifc_len)
				break;
			memset(&entries[nr], 0, sizeof(entries[nr]));
			strlcpy(entries[nr].ifr_name, dev->name, IFNAMSIZ);
			ifreq_set_addr(&entries[nr], dev->ip_addr);
			nr++;
		}
		spin_unlock_irqrestore(&netdev_lock, flags);

		bytes = nr * sizeof(struct ifreq);
		if (bytes && copy_to_user(ifc.ifc_buf, entries, bytes))
			return -EFAULT;
		ifc.ifc_len = (int)bytes;
		if (copy_to_user((void *)arg, &ifc, sizeof(ifc)))
			return -EFAULT;
		return 0;
	}

	if (cmd == SIOCGIFSTATS) {
		struct ifstats stats;

		if (copy_from_user(&stats, (void *)arg, sizeof(stats)))
			return -EFAULT;
		stats.ifs_name[IFNAMSIZ - 1] = '\0';
		dev = netdev_get_by_name(stats.ifs_name);
		if (!dev)
			return -ENODEV;
		stats.rx_packets = dev->stats.rx_packets;
		stats.tx_packets = dev->stats.tx_packets;
		stats.rx_bytes = dev->stats.rx_bytes;
		stats.tx_bytes = dev->stats.tx_bytes;
		stats.rx_errors = dev->stats.rx_errors;
		stats.tx_errors = dev->stats.tx_errors;
		stats.rx_dropped = dev->stats.rx_dropped;
		stats.tx_dropped = dev->stats.tx_dropped;
		if (copy_to_user((void *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;
	}

	if (cmd == SIOCADDRT || cmd == SIOCDELRT) {
		if (copy_from_user(&rt, (void *)arg, sizeof(rt)))
			return -EFAULT;
		if (!rt.rt_dev[0])
			return -EINVAL;
		dev = netdev_get_by_name(rt.rt_dev);
		if (!dev)
			return -ENODEV;
		if (sockaddr_to_ipv4(&rt.rt_dst, &addr) ||
		    sockaddr_to_ipv4(&rt.rt_genmask, &mask) ||
		    sockaddr_to_ipv4(&rt.rt_gateway, &gw))
			return -EINVAL;
		if (cmd == SIOCADDRT)
			return route_add(addr, mask, gw, dev);
		return route_del(addr, mask, gw);
	}

	if (copy_from_user(&ifr, (void *)arg, sizeof(ifr)))
		return -EFAULT;
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	dev = netdev_get_by_name(ifr.ifr_name);
	if (!dev)
		return -ENODEV;

	switch (cmd) {
	case SIOCGIFADDR:
		ifreq_set_addr(&ifr, dev->ip_addr);
		copy_ifr = true;
		break;
	case SIOCSIFADDR:
		err = ifreq_get_addr(&ifr, &addr);
		if (err)
			return err;
		return netdev_set_ipv4(dev, addr, dev->netmask, 0);
	case SIOCGIFNETMASK:
		ifreq_set_addr(&ifr, dev->netmask);
		copy_ifr = true;
		break;
	case SIOCSIFNETMASK:
		err = ifreq_get_addr(&ifr, &mask);
		if (err)
			return err;
		return netdev_set_ipv4(dev, dev->ip_addr, mask, 0);
	case SIOCGIFBRDADDR:
		ifreq_set_addr(&ifr, dev->broadcast);
		copy_ifr = true;
		break;
	case SIOCSIFBRDADDR:
		err = ifreq_get_addr(&ifr, &addr);
		if (err)
			return err;
		return netdev_set_ipv4(dev, dev->ip_addr, dev->netmask, addr);
	case SIOCGIFFLAGS:
		ifr.ifr_flags = (short)dev->flags;
		copy_ifr = true;
		break;
	case SIOCSIFFLAGS:
		return netdev_set_up(dev, (ifr.ifr_flags & IFF_UP) != 0);
	case SIOCGIFHWADDR:
		ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
		memcpy(ifr.ifr_hwaddr.sa_data, dev->dev_addr, ETH_ALEN);
		copy_ifr = true;
		break;
	case SIOCGIFMTU:
		ifr.ifr_mtu = (int)dev->mtu;
		copy_ifr = true;
		break;
	case SIOCSIFMTU:
		if (ifr.ifr_mtu < 68 || ifr.ifr_mtu > 65535)
			return -EINVAL;
		dev->mtu = (unsigned int)ifr.ifr_mtu;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (copy_ifr && copy_to_user((void *)arg, &ifr, sizeof(ifr)))
		return -EFAULT;
	return 0;
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

static int netif_rx_deliver(struct sk_buff *skb)
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

void netif_rx_init(void)
{
	skb_queue_init(&net_rx_queue);
}

static void net_rx_thread(void *arg)
{
	(void)arg;

	for (;;) {
		struct sk_buff *skb;
		irqstate_t flags;

		flags = spin_lock_irqsave(&net_rx_queue_lock);
		while (skb_queue_empty(&net_rx_queue))
			task_sleep(&net_rx_wait, &net_rx_queue_lock);
		skb = skb_dequeue(&net_rx_queue);
		spin_unlock_irqrestore(&net_rx_queue_lock, flags);

		if (skb)
			netif_rx_deliver(skb);
	}
}

int netif_rx_start(void)
{
	struct task *task;

	if (net_rx_started)
		return 0;

	task = kthread_create(net_rx_thread, NULL);
	if (!task)
		return -ENOMEM;

	strlcpy(task->name, "net-rx", sizeof(task->name));
	kthread_join(task);
	net_rx_started = true;
	return 0;
}

int netif_rx(struct sk_buff *skb)
{
	irqstate_t flags;

	if (!skb || !skb->dev) {
		skb_free(skb);
		return -EINVAL;
	}

	flags = spin_lock_irqsave(&net_rx_queue_lock);
	if (net_rx_queue.qlen >= NET_RX_QUEUE_MAX) {
		skb->dev->stats.rx_dropped++;
		spin_unlock_irqrestore(&net_rx_queue_lock, flags);
		skb_free(skb);
		return -ENOBUFS;
	}
	skb_queue_tail(&net_rx_queue, skb);
	task_wake_all(&net_rx_wait);
	spin_unlock_irqrestore(&net_rx_queue_lock, flags);
	return 0;
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
