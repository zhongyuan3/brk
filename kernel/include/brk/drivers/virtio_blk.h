#ifndef BRK_VIRTIO_BLK_H
#define BRK_VIRTIO_BLK_H

#include <brk/base/types.h>
#include <brk/drivers/virtio_queue.h>
#include <brk/lock/spinlock_types.h>

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14
#define VIRTIO_BLK_F_LIFETIME 15
#define VIRTIO_BLK_F_SECURE_ERASE 16
#define VIRTIO_BLK_F_ZONED 17
#define VIRTIO_BLK_F_BARRIER 0
#define VIRTIO_BLK_F_SCSI 7

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_GET_ID 8
#define VIRTIO_BLK_T_GET_LIFETIME 10
#define VIRTIO_BLK_T_DISCARD 11
#define VIRTIO_BLK_T_WRITE_ZEROES 13
#define VIRTIO_BLK_T_SECURE_ERASE 14

#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

struct virtio_blk_req {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

struct virtio_blk_config {
	/* The capacity (in 512-byte sectors). */
	uint64_t capacity;
	/* The maximum segment size (if VIRTIO_BLK_F_SIZE_MAX) */
	uint32_t size_max;
	/* The maximum number of segments (if VIRTIO_BLK_F_SEG_MAX) */
	uint32_t seg_max;
	/* geometry of the device (if VIRTIO_BLK_F_GEOMETRY) */
	struct virtio_blk_geometry {
		uint16_t cylinders;
		uint8_t heads;
		uint8_t sectors;
	} geometry;

	/* block size of device (if VIRTIO_BLK_F_BLK_SIZE) */
	uint32_t blk_size;

	/* the next 4 entries are guarded by VIRTIO_BLK_F_TOPOLOGY  */
	/* exponent for physical block per logical block. */
	uint8_t physical_block_exp;
	/* alignment offset in logical blocks. */
	uint8_t alignment_offset;
	/* minimum I/O size without performance penalty in logical blocks. */
	uint16_t min_io_size;
	/* optimal sustained I/O size in logical blocks. */
	uint32_t opt_io_size;

	/* writeback mode (if VIRTIO_BLK_F_CONFIG_WCE) */
	uint8_t wce;
	uint8_t unused;

	/* number of vqs, only available when VIRTIO_BLK_F_MQ is set */
	uint16_t num_queues;

	/* the next 3 entries are guarded by VIRTIO_BLK_F_DISCARD */
	/*
	 * The maximum discard sectors (in 512-byte sectors) for
	 * one segment.
	 */
	uint32_t max_discard_sectors;
	/*
	 * The maximum number of discard segments in a
	 * discard command.
	 */
	uint32_t max_discard_seg;
	/* Discard commands must be aligned to this number of sectors. */
	uint32_t discard_sector_alignment;

	/* the next 3 entries are guarded by VIRTIO_BLK_F_WRITE_ZEROES */
	/*
	 * The maximum number of write zeroes sectors (in 512-byte sectors) in
	 * one segment.
	 */
	uint32_t max_write_zeroes_sectors;
	/*
	 * The maximum number of segments in a write zeroes
	 * command.
	 */
	uint32_t max_write_zeroes_seg;
	/*
	 * Set if a VIRTIO_BLK_T_WRITE_ZEROES request may result in the
	 * deallocation of one or more of the sectors.
	 */
	uint8_t write_zeroes_may_unmap;

	uint8_t unused1[3];

	/* the next 3 entries are guarded by VIRTIO_BLK_F_SECURE_ERASE */
	/*
	 * The maximum secure erase sectors (in 512-byte sectors) for
	 * one segment.
	 */
	uint32_t max_secure_erase_sectors;
	/*
	 * The maximum number of secure erase segments in a
	 * secure erase command.
	 */
	uint32_t max_secure_erase_seg;
	/* Secure erase commands must be aligned to this number of sectors. */
	uint32_t secure_erase_sector_alignment;

	/* Zoned block device characteristics (if VIRTIO_BLK_F_ZONED) */
	struct virtio_blk_zoned_characteristics {
		uint32_t zone_sectors;
		uint32_t max_open_zones;
		uint32_t max_active_zones;
		uint32_t max_append_sectors;
		uint32_t write_granularity;
		uint8_t model;
		uint8_t unused2[3];
	} zoned;
} __attribute__((packed));

#define VIRTIO_BLK_SECTOR_SIZE 512
#define VIRTIO_BLK_DEFAULT_QUEUE_SIZE 16

struct virtio_blk_io_desc {
	uint64_t buf_phys;
	uint64_t sector;
	size_t sec_count;
	bool is_write;
	bool completed;
};

struct virtio_blk_slot {
	struct virtio_blk_io_desc *trans;
	char status;
};

struct virtio_blk_dev {
	struct virtio_device *vdev;
	struct virtq vq;
	struct virtio_blk_req *reqs;
	struct virtio_blk_slot *slots;
	unsigned queue_size;
	spinlock_t lock;
	struct virtio_blk_config config;
};

struct virtio_blk_registry {
	struct virtio_blk_dev **vblks;
	struct block_dev **bdevs;
	unsigned num_vblks;
	spinlock_t lock;
	unsigned major;
	unsigned minor_start;
};

int virtio_blk_init(void);
struct virtio_blk_dev *virtio_blk_create(struct virtio_device *vdev,
					 unsigned queue_size);
void virtio_blk_destroy(struct virtio_blk_dev *vblk);
int virtio_blk_register(struct virtio_blk_dev *vblk);
void virtio_blk_unregister(struct virtio_blk_dev *vblk);
int virtio_blk_enable_irq(uint32_t hart_id);
int virtio_blk_mknod(void);

#endif
