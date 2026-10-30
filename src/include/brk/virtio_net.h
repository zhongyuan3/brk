#ifndef BRK_VIRTIO_NET_H
#define BRK_VIRTIO_NET_H

#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <brk/virtio.h>
#include <brk/virtio_queue.h>

/*
 * Feature bits defined by the virtio network device (low 32 bits).
 * The driver deliberately negotiates only MAC/MTU/STATUS and disables
 * every offload, mergeable-buffer and control-queue feature.
 */
#define VIRTIO_NET_F_CSUM 0
#define VIRTIO_NET_F_GUEST_CSUM 1
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS 2
#define VIRTIO_NET_F_MTU 3
#define VIRTIO_NET_F_MAC 5
#define VIRTIO_NET_F_GSO 6
#define VIRTIO_NET_F_GUEST_TSO4 7
#define VIRTIO_NET_F_GUEST_TSO6 8
#define VIRTIO_NET_F_GUEST_ECN 9
#define VIRTIO_NET_F_GUEST_UFO 10
#define VIRTIO_NET_F_HOST_TSO4 11
#define VIRTIO_NET_F_HOST_TSO6 12
#define VIRTIO_NET_F_HOST_ECN 13
#define VIRTIO_NET_F_HOST_UFO 14
#define VIRTIO_NET_F_MRG_RXBUF 15
#define VIRTIO_NET_F_STATUS 16
#define VIRTIO_NET_F_CTRL_VQ 17
#define VIRTIO_NET_F_CTRL_RX 18
#define VIRTIO_NET_F_CTRL_VLAN 19
#define VIRTIO_NET_F_CTRL_RX_EXTRA 20
#define VIRTIO_NET_F_GUEST_ANNOUNCE 21
#define VIRTIO_NET_F_MQ 22
#define VIRTIO_NET_F_CTRL_MAC_ADDR 23

#define VIRTIO_NET_RX_QUEUE 0
#define VIRTIO_NET_TX_QUEUE 1

#define VIRTIO_NET_QUEUE_SIZE 64
#define VIRTIO_NET_BUF_SIZE 2048
#define VIRTIO_NET_MAX_FRAME 1514

/* The legacy/no-MRG_RXBUF header is 10 bytes. */
struct virtio_net_hdr {
	u8 flags;
	u8 gso_type;
	u16 hdr_len;
	u16 gso_size;
	u16 csum_start;
	u16 csum_offset;
} __attribute__((packed));

struct virtio_net_dev;
struct net_device;

/*
 * Called from interrupt context after a frame has been copied out of the
 * receive ring.  @frame is owned by the driver and is freed as soon as the
 * callback returns, so a consumer must copy it if it needs to keep it.
 */
typedef void (*virtio_net_rx_fn)(struct virtio_net_dev *vnet, const void *frame,
				 size_t len, void *ctx);

struct virtio_net_dev {
	struct virtio_device *vdev;
	struct net_device *ndev;
	struct virtq rx_vq;
	struct virtq tx_vq;

	/* Indexed by virtqueue descriptor id. */
	u8 *rx_buf[VIRTIO_NET_QUEUE_SIZE];
	u8 *tx_buf[VIRTIO_NET_QUEUE_SIZE];

	u8 mac[6];
	unsigned int mtu;

	spinlock_t lock;
	virtio_net_rx_fn rx_cb;
	void *rx_ctx;

	u64 rx_packets;
	u64 tx_packets;
	u64 rx_bytes;
	u64 tx_bytes;
	u64 rx_dropped;
	u64 tx_dropped;
};

/*
 * virtio_net_create() - initialize a virtio-net device
 * @vdev: the virtio device; its MMIO transport must already be probed
 *
 * Returns a device on success, or NULL on failure.
 */
struct virtio_net_dev *virtio_net_create(struct virtio_device *vdev);
void virtio_net_destroy(struct virtio_net_dev *vnet);

void virtio_net_set_rx_callback(struct virtio_net_dev *vnet,
				virtio_net_rx_fn fn, void *ctx);

/*
 * virtio_net_transmit() - queue one Ethernet frame for transmission
 * @frame: complete Ethernet frame, including the 14-byte header
 * @len: frame length, at most VIRTIO_NET_MAX_FRAME
 *
 * This call never sleeps: if the transmit ring is full it drops the frame
 * and returns -ENOBUFS.  It may be called from interrupt context.
 */
int virtio_net_transmit(struct virtio_net_dev *vnet, const void *frame,
			size_t len);

const u8 *virtio_net_get_mac(struct virtio_net_dev *vnet);

/* Return the virq of the first probed virtio-net device (0 if none). */
unsigned int virtio_net_first_irq(void);

#endif
