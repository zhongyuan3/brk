#include <arch/mm.h>
#include <brk/device_model.h>
#include <brk/dt.h>
#include <brk/initcall.h>
#include <brk/irq.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/net/eth.h>
#include <brk/net/net.h>
#include <brk/net/netdevice.h>
#include <brk/net/route.h>
#include <brk/net/skbuff.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_mmio.h>
#include <brk/virtio_net.h>
#include <brk/virtio_queue.h>
#include <uapi/errno.h>

#define VIRTIO_NET_MAX_DEVS 4

static struct virtio_net_dev *vnet_devs[VIRTIO_NET_MAX_DEVS];
static SPINLOCK_DEFINE(vnet_registry_lock);
static unsigned int vnet_index;

struct vnet_rx_pkt {
	u8 *data;
	u32 len;
};

struct vnet_rx_collect {
	struct virtio_net_dev *vnet;
	struct vnet_rx_pkt *pkts;
	unsigned int nr;
	unsigned int max;
};

static u32 vnet_select_features(struct virtio_device *vdev)
{
	u32 features = virtio_mmio_read_features(vdev);

	/* No checksum/TSO/UFO offloads. */
	features &= ~(1u << VIRTIO_NET_F_CSUM);
	features &= ~(1u << VIRTIO_NET_F_GUEST_CSUM);
	features &= ~(1u << VIRTIO_NET_F_CTRL_GUEST_OFFLOADS);
	features &= ~(1u << VIRTIO_NET_F_GSO);
	features &= ~(1u << VIRTIO_NET_F_GUEST_TSO4);
	features &= ~(1u << VIRTIO_NET_F_GUEST_TSO6);
	features &= ~(1u << VIRTIO_NET_F_GUEST_ECN);
	features &= ~(1u << VIRTIO_NET_F_GUEST_UFO);
	features &= ~(1u << VIRTIO_NET_F_HOST_TSO4);
	features &= ~(1u << VIRTIO_NET_F_HOST_TSO6);
	features &= ~(1u << VIRTIO_NET_F_HOST_ECN);
	features &= ~(1u << VIRTIO_NET_F_HOST_UFO);

	/* One buffer per packet; no mergeable receive buffers. */
	features &= ~(1u << VIRTIO_NET_F_MRG_RXBUF);

	/* No control virtqueue and no multi-queue. */
	features &= ~(1u << VIRTIO_NET_F_CTRL_VQ);
	features &= ~(1u << VIRTIO_NET_F_CTRL_RX);
	features &= ~(1u << VIRTIO_NET_F_CTRL_RX_EXTRA);
	features &= ~(1u << VIRTIO_NET_F_CTRL_VLAN);
	features &= ~(1u << VIRTIO_NET_F_GUEST_ANNOUNCE);
	features &= ~(1u << VIRTIO_NET_F_MQ);
	features &= ~(1u << VIRTIO_NET_F_CTRL_MAC_ADDR);

	/* Keep the ring interface dead simple. */
	features &= ~(1u << VIRTIO_F_EVENT_IDX);
	features &= ~(1u << VIRTIO_F_INDIRECT_DESC);
	features &= ~(1u << VIRTIO_F_ANY_LAYOUT);
	features &= ~(1u << VIRTIO_F_NOTIFY_ON_EMPTY);

	return features;
}

static bool vnet_mac_valid(const u8 *mac)
{
	bool any = false;
	bool all_ff = true;

	for (unsigned int i = 0; i < 6; i++) {
		if (mac[i])
			any = true;
		if (mac[i] != 0xff)
			all_ff = false;
	}

	return any && !all_ff && !(mac[0] & 0x01);
}

static void vnet_generate_mac(struct virtio_net_dev *vnet)
{
	unsigned int index =
		__atomic_fetch_add(&vnet_index, 1, __ATOMIC_RELAXED);

	/* Locally administered, unicast. */
	vnet->mac[0] = 0x02;
	vnet->mac[1] = 0x00;
	vnet->mac[2] = 0x00;
	vnet->mac[3] = 0x00;
	vnet->mac[4] = (index >> 8) & 0xff;
	vnet->mac[5] = index & 0xff;
}

static void vnet_read_config(struct virtio_net_dev *vnet, u32 features)
{
	u16 mtu = 1500;

	memset(vnet->mac, 0, sizeof(vnet->mac));
	if (features & (1u << VIRTIO_NET_F_MAC))
		virtio_mmio_read(vnet->vdev, vnet->mac, sizeof(vnet->mac),
				 VIRTIO_CONFIG_OFFSET);

	if (!vnet_mac_valid(vnet->mac))
		vnet_generate_mac(vnet);

	/*
	 * virtio_net_config: mac[6], status(u16), max_virtqueue_pairs(u16),
	 * mtu(u16).  mtu is only valid with VIRTIO_NET_F_MTU.
	 */
	if (features & (1u << VIRTIO_NET_F_MTU)) {
		if (!virtio_mmio_read(vnet->vdev, &mtu, sizeof(mtu),
				      VIRTIO_CONFIG_OFFSET + 10) &&
		    mtu >= 576)
			vnet->mtu = mtu;
	}
}

static void vnet_free_bufs(struct virtio_net_dev *vnet)
{
	for (unsigned int i = 0; i < VIRTIO_NET_QUEUE_SIZE; i++) {
		kfree(vnet->rx_buf[i]);
		vnet->rx_buf[i] = NULL;
		kfree(vnet->tx_buf[i]);
		vnet->tx_buf[i] = NULL;
	}
}

static int vnet_alloc_tx_bufs(struct virtio_net_dev *vnet)
{
	for (unsigned int i = 0; i < VIRTIO_NET_QUEUE_SIZE; i++) {
		vnet->tx_buf[i] = kmalloc(VIRTIO_NET_BUF_SIZE);
		if (!vnet->tx_buf[i])
			return -ENOMEM;
	}
	return 0;
}

/*
 * Fill the receive ring.  Buffers are pre-allocated and indexed by their
 * descriptor id, so the used-ring callback can find them in O(1).  The
 * device is notified by the caller once DRIVER_OK has been set.
 */
static int vnet_post_rx(struct virtio_net_dev *vnet)
{
	for (unsigned int i = 0; i < VIRTIO_NET_QUEUE_SIZE; i++) {
		struct virtq_desc *desc;
		unsigned int idx;
		u8 *buf;
		int err;

		err = virtq_alloc_desc(&vnet->rx_vq, &idx);
		if (err)
			return err;

		buf = kmalloc(VIRTIO_NET_BUF_SIZE);
		if (!buf) {
			virtq_free_desc(&vnet->rx_vq, idx);
			return -ENOMEM;
		}
		vnet->rx_buf[idx] = buf;

		desc = &vnet->rx_vq.desc[idx];
		desc->addr = virt_to_phys((uintptr_t)buf);
		desc->len = VIRTIO_NET_BUF_SIZE;
		desc->flags = VIRTQ_DESC_F_WRITE;
		desc->next = 0;

		virtq_submit(&vnet->rx_vq, idx);
	}

	return 0;
}

static void vnet_rx_used(struct virtq *vq, u32 id, u32 len, void *ctx)
{
	struct vnet_rx_collect *c = ctx;
	struct virtio_net_dev *vnet = c->vnet;
	const u32 hdr_len = sizeof(struct virtio_net_hdr);
	u32 frame_len;

	if (id >= VIRTIO_NET_QUEUE_SIZE || !vnet->rx_buf[id]) {
		vnet->rx_dropped++;
		return;
	}

	if (len <= hdr_len)
		goto drop;

	frame_len = len - hdr_len;
	if (frame_len > VIRTIO_NET_MAX_FRAME)
		goto drop;

	if (c->nr >= c->max)
		goto drop;

	u8 *copy = kmalloc(frame_len);
	if (!copy)
		goto drop;

	memcpy(copy, vnet->rx_buf[id] + hdr_len, frame_len);

	c->pkts[c->nr].data = copy;
	c->pkts[c->nr].len = frame_len;
	c->nr++;

	/* Re-arm the same descriptor for the next packet. */
	virtq_submit(vq, id);
	return;

drop:
	vnet->rx_dropped++;
	virtq_submit(vq, id);
}

static void vnet_tx_used(struct virtq *vq, u32 id, u32 len, void *ctx)
{
	(void)len;
	(void)ctx;

	if (id < VIRTIO_NET_QUEUE_SIZE)
		virtq_free_desc(vq, id);
}

static irqreturn_t vnet_irq(int irq, void *dev_id)
{
	struct virtio_net_dev *vnet = dev_id;
	struct vnet_rx_pkt pkts[VIRTIO_NET_QUEUE_SIZE];
	struct vnet_rx_collect collect = {
		.vnet = vnet,
		.pkts = pkts,
		.nr = 0,
		.max = VIRTIO_NET_QUEUE_SIZE,
	};
	virtio_net_rx_fn cb;
	void *cb_ctx;
	u32 status;
	irqstate_t flags;

	(void)irq;

	flags = spin_lock_irqsave(&vnet->lock);

	status = virtio_mmio_ack_interrupt(vnet->vdev);
	__sync_synchronize();

	/* Bit 0 covers used-ring updates from every queue. */
	if (status & 1) {
		virtq_process_used(&vnet->rx_vq, vnet_rx_used, &collect);
		virtq_process_used(&vnet->tx_vq, vnet_tx_used, vnet);
	}

	cb = vnet->rx_cb;
	cb_ctx = vnet->rx_ctx;

	spin_unlock_irqrestore(&vnet->lock, flags);

	vnet->rx_packets += collect.nr;

	for (unsigned int i = 0; i < collect.nr; i++) {
		struct vnet_rx_pkt *pkt = &pkts[i];

		vnet->rx_bytes += pkt->len;
		if (cb)
			cb(vnet, pkt->data, pkt->len, cb_ctx);
		else
			vnet->rx_dropped++;
		kfree(pkt->data);
	}
	return IRQ_HANDLED;
}

static int vnet_ndo_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct virtio_net_dev *vnet = netdev_priv(dev);
	int err;

	err = virtio_net_transmit(vnet, skb->data, skb->len);
	skb_free(skb);
	return err;
}

static const struct net_device_ops vnet_netdev_ops = {
	.ndo_start_xmit = vnet_ndo_start_xmit,
};

static void vnet_rx_to_stack(struct virtio_net_dev *vnet, const void *frame,
			     size_t len, void *ctx)
{
	struct net_device *dev = ctx;
	struct sk_buff *skb;

	(void)vnet;

	skb = skb_alloc(len);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}

	memcpy(skb_put(skb, len), frame, len);
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	if (!skb->protocol) {
		dev->stats.rx_errors++;
		skb_free(skb);
		return;
	}

	netif_rx(skb);
}

static struct net_device *vnet_netdev_create(struct virtio_net_dev *vnet)
{
	static unsigned int index;
	char name[IFNAMSIZ];
	struct net_device *dev;

	snprintf(name, sizeof(name), "eth%u", index++);
	dev = netdev_alloc(name);
	if (!dev)
		return NULL;

	memcpy(dev->dev_addr, vnet->mac, ETH_ALEN);
	dev->type = ARPHRD_ETHER;
	dev->mtu = vnet->mtu;
	dev->flags = IFF_UP | IFF_BROADCAST;
	dev->netdev_ops = &vnet_netdev_ops;
	dev->priv = vnet;
	dev->ip_addr = NET_DEFAULT_IP;
	dev->netmask = NET_DEFAULT_NETMASK;
	dev->broadcast = NET_DEFAULT_IP | ~NET_DEFAULT_NETMASK;
	vnet->ndev = dev;
	return dev;
}

struct virtio_net_dev *virtio_net_create(struct virtio_device *vdev)
{
	struct virtio_net_dev *vnet;
	u32 features;
	int err;

	if (!vdev)
		return NULL;

	vnet = kzalloc(sizeof(*vnet));
	if (!vnet)
		return NULL;

	spinlock_init(&vnet->lock, "virtio_net_dev");
	vnet->vdev = vdev;
	vnet->mtu = 1500;

	virtio_mmio_reset(vdev);
	err = virtio_mmio_start_driver(vdev);
	if (err)
		goto err_free;

	features = vnet_select_features(vdev);
	virtio_mmio_write_features(vdev, features);
	err = virtio_mmio_features_ok(vdev);
	if (err)
		goto err_free;

	err = virtq_alloc(&vnet->rx_vq, VIRTIO_NET_QUEUE_SIZE);
	if (err)
		goto err_free;
	err = virtq_alloc(&vnet->tx_vq, VIRTIO_NET_QUEUE_SIZE);
	if (err)
		goto err_free_rx_vq;

	err = virtio_mmio_setup_queue(vdev, VIRTIO_NET_RX_QUEUE, &vnet->rx_vq,
				      VIRTIO_NET_QUEUE_SIZE);
	if (err)
		goto err_free_tx_vq;
	err = virtio_mmio_setup_queue(vdev, VIRTIO_NET_TX_QUEUE, &vnet->tx_vq,
				      VIRTIO_NET_QUEUE_SIZE);
	if (err)
		goto err_free_tx_vq;

	vnet_read_config(vnet, features);

	err = vnet_alloc_tx_bufs(vnet);
	if (err)
		goto err_free_bufs;

	err = vnet_post_rx(vnet);
	if (err)
		goto err_free_bufs;

	err = request_irq(vdev->irq, vnet_irq, IRQF_SHARED, "virtio_net",
			  vnet);
	if (err)
		goto err_free_bufs;

	err = virtio_mmio_driver_ok(vdev);
	if (err) {
		free_irq(vdev->irq, vnet);
		goto err_free_bufs;
	}

	vnet->ndev = vnet_netdev_create(vnet);
	if (!vnet->ndev) {
		err = -ENOMEM;
		goto err_reset;
	}

	err = netdev_register(vnet->ndev);
	if (err) {
		netdev_free(vnet->ndev);
		vnet->ndev = NULL;
		goto err_reset;
	}

	route_add(NET_DEFAULT_IP & NET_DEFAULT_NETMASK, NET_DEFAULT_NETMASK, 0,
		  vnet->ndev);
	route_add(0, 0, NET_DEFAULT_GW, vnet->ndev);

	virtio_net_set_rx_callback(vnet, vnet_rx_to_stack, vnet->ndev);

	/* Now the device may use the receive ring. */
	virtio_mmio_queue_notify(vdev, VIRTIO_NET_RX_QUEUE);

	pr_info("virtio_net: mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%u\n",
		vnet->mac[0], vnet->mac[1], vnet->mac[2], vnet->mac[3],
		vnet->mac[4], vnet->mac[5], vnet->mtu);
	return vnet;

err_reset:
	virtio_mmio_reset(vdev);
	free_irq(vdev->irq, vnet);
err_free_bufs:
	vnet_free_bufs(vnet);
err_free_tx_vq:
	virtq_free(&vnet->tx_vq);
err_free_rx_vq:
	virtq_free(&vnet->rx_vq);
err_free:
	kfree(vnet);
	return NULL;
}

void virtio_net_destroy(struct virtio_net_dev *vnet)
{
	if (!vnet)
		return;

	if (vnet->ndev) {
		virtio_net_set_rx_callback(vnet, NULL, NULL);
		netdev_unregister(vnet->ndev);
		netdev_free(vnet->ndev);
		vnet->ndev = NULL;
	}

	if (vnet->vdev) {
		/* Stop the device before its descriptor memory disappears. */
		virtio_mmio_reset(vnet->vdev);
		free_irq(vnet->vdev->irq, vnet);
	}

	vnet_free_bufs(vnet);
	virtq_free(&vnet->tx_vq);
	virtq_free(&vnet->rx_vq);
	kfree(vnet);
}

void virtio_net_set_rx_callback(struct virtio_net_dev *vnet,
				virtio_net_rx_fn fn, void *ctx)
{
	irqstate_t flags;

	if (!vnet)
		return;

	flags = spin_lock_irqsave(&vnet->lock);
	vnet->rx_cb = fn;
	vnet->rx_ctx = ctx;
	spin_unlock_irqrestore(&vnet->lock, flags);
}

int virtio_net_transmit(struct virtio_net_dev *vnet, const void *frame,
			size_t len)
{
	const u32 hdr_len = sizeof(struct virtio_net_hdr);
	struct virtq_desc *desc;
	unsigned int idx;
	u8 *buf;
	irqstate_t flags;
	int err;

	if (!vnet || !frame || !len)
		return -EINVAL;
	if (len > VIRTIO_NET_MAX_FRAME)
		return -EMSGSIZE;

	flags = spin_lock_irqsave(&vnet->lock);

	err = virtq_alloc_desc(&vnet->tx_vq, &idx);
	if (err) {
		vnet->tx_dropped++;
		spin_unlock_irqrestore(&vnet->lock, flags);
		return -ENOBUFS;
	}

	buf = vnet->tx_buf[idx];
	memset(buf, 0, hdr_len);
	memcpy(buf + hdr_len, frame, len);

	desc = &vnet->tx_vq.desc[idx];
	desc->addr = virt_to_phys((uintptr_t)buf);
	desc->len = hdr_len + len;
	desc->flags = 0;
	desc->next = 0;

	virtq_submit(&vnet->tx_vq, idx);
	virtio_mmio_queue_notify(vnet->vdev, VIRTIO_NET_TX_QUEUE);

	vnet->tx_packets++;
	vnet->tx_bytes += len;

	spin_unlock_irqrestore(&vnet->lock, flags);
	return 0;
}

const u8 *virtio_net_get_mac(struct virtio_net_dev *vnet)
{
	return vnet ? vnet->mac : NULL;
}

unsigned int virtio_net_first_irq(void)
{
	unsigned int irq = 0;
	irqstate_t flags = spin_lock_irqsave(&vnet_registry_lock);

	for (unsigned int i = 0; i < VIRTIO_NET_MAX_DEVS; i++) {
		if (vnet_devs[i]) {
			irq = vnet_devs[i]->vdev->irq;
			break;
		}
	}

	spin_unlock_irqrestore(&vnet_registry_lock, flags);
	return irq;
}

static int virtio_net_probe(struct device *dev)
{
	struct virtio_device *vdev =
		container_of(dev, struct virtio_device, dev);
	struct virtio_net_dev *vnet;
	irqstate_t flags;
	unsigned int slot;

	vnet = virtio_net_create(vdev);
	if (!vnet)
		return -ENOMEM;

	flags = spin_lock_irqsave(&vnet_registry_lock);
	for (slot = 0; slot < VIRTIO_NET_MAX_DEVS; slot++) {
		if (!vnet_devs[slot])
			break;
	}
	if (slot == VIRTIO_NET_MAX_DEVS) {
		spin_unlock_irqrestore(&vnet_registry_lock, flags);
		virtio_net_destroy(vnet);
		return -EBUSY;
	}
	vnet_devs[slot] = vnet;
	spin_unlock_irqrestore(&vnet_registry_lock, flags);

	dev->driver_data = vnet;
	return 0;
}

static void virtio_net_remove(struct device *dev)
{
	struct virtio_net_dev *vnet = dev->driver_data;
	irqstate_t flags;

	if (!vnet)
		return;
	dev->driver_data = NULL;

	flags = spin_lock_irqsave(&vnet_registry_lock);
	for (unsigned int i = 0; i < VIRTIO_NET_MAX_DEVS; i++) {
		if (vnet_devs[i] == vnet) {
			vnet_devs[i] = NULL;
			break;
		}
	}
	spin_unlock_irqrestore(&vnet_registry_lock, flags);

	virtio_net_destroy(vnet);
}

static const struct device_id virtio_net_ids[] = {
	{ .device_id = VIRTIO_DEVICE_ID_NET },
	{},
};

static struct device_driver virtio_net_driver = {
	.name = "virtio_net",
	.bus = &virtio_bus,
	.id_table = virtio_net_ids,
	.probe = virtio_net_probe,
	.remove = virtio_net_remove,
};

void virtio_net_init(void)
{
	int err;

	/*
	 * virtio_blk_init() already scans the device tree at postcore time,
	 * but the scan is idempotent so this also works when only the network
	 * driver is built in.
	 */
	dt_init_scan_virtio_dev();

	err = driver_register(&virtio_net_driver);
	if (err)
		panic("%s(): driver_register failed: %d\n", __func__, err);
}
subsys_initcall(virtio_net_init);
