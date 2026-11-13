#ifndef BRK_NETDEVICE_H
#define BRK_NETDEVICE_H

#include <brk/device.h>
#include <brk/list.h>
#include <brk/skbuff.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

#define IFNAMSIZ 16

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8

#define ARPHRD_ETHER 1
#define ARPHRD_LOOPBACK 772

struct net_device_stats {
	u64 rx_packets;
	u64 tx_packets;
	u64 rx_bytes;
	u64 tx_bytes;
	u64 rx_dropped;
	u64 tx_dropped;
	u64 rx_errors;
	u64 tx_errors;
};

struct net_device_ops {
	/*
	 * Consumes @skb in every case, including errors.  Must never sleep:
	 * it may be called from interrupt context.
	 */
	int (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev);
	int (*ndo_open)(struct net_device *dev);
	int (*ndo_stop)(struct net_device *dev);
};

struct net_device {
	char name[IFNAMSIZ];
	struct device dev;
	u8 dev_addr[6];
	unsigned int mtu;
	unsigned int flags;
	unsigned short type;
	unsigned int ifindex;

	/* IPv4 configuration; host byte order. */
	u32 ip_addr;
	u32 netmask;
	u32 broadcast;

	const struct net_device_ops *netdev_ops;
	struct net_device_stats stats;
	spinlock_t tx_lock;
	struct list_head dev_list;
	void *priv;
};

struct packet_type {
	u16 type; /* host byte order ETH_P_*; ETH_P_ALL matches everything */
	struct net_device *dev; /* NULL: any device */
	int (*func)(struct sk_buff *skb, struct net_device *dev,
		    struct packet_type *pt);
	struct list_head list;
};

struct net_device *netdev_alloc(const char *name);
void netdev_free(struct net_device *dev);
int netdev_register(struct net_device *dev);
void netdev_unregister(struct net_device *dev);
struct net_device *netdev_get_by_name(const char *name);
struct net_device *netdev_get_broadcast_dev(void);

int netdev_set_up(struct net_device *dev, bool up);
int netdev_set_ipv4(struct net_device *dev, u32 addr, u32 mask, u32 broadcast);

/* Handle SIOC* network configuration ioctls from a user pointer. */
int netdev_ioctl(unsigned int cmd, unsigned long arg);

static inline void *netdev_priv(const struct net_device *dev)
{
	return dev->priv;
}

/* Queue a received frame for deferred protocol processing.  Consumes @skb. */
int netif_rx(struct sk_buff *skb);

/* Initialize and start the deferred RX worker. */
void netif_rx_init(void);
int netif_rx_start(void);

/* Transmit @skb through skb->dev.  Consumes @skb. */
int netdev_start_xmit(struct sk_buff *skb);

void dev_add_pack(struct packet_type *pt);
void dev_remove_pack(struct packet_type *pt);

int loopback_init(void);

#endif
