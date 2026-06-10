#ifndef BRK_VIRTIO_BLK_H
#define BRK_VIRTIO_BLK_H

#include <brk/drivers/virtio_queue.h>
#include <brk/lib/types.h>
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
	u32 type;
	u32 reserved;
	u64 sector;
};

struct virtio_blk_config {
	/* The capacity (in 512-byte sectors). */
	u64 capacity;
	/* The maximum segment size (if VIRTIO_BLK_F_SIZE_MAX) */
	u32 size_max;
	/* The maximum number of segments (if VIRTIO_BLK_F_SEG_MAX) */
	u32 seg_max;
	/* geometry of the device (if VIRTIO_BLK_F_GEOMETRY) */
	struct virtio_blk_geometry {
		u16 cylinders;
		u8 heads;
		u8 sectors;
	} geometry;

	/* block size of device (if VIRTIO_BLK_F_BLK_SIZE) */
	u32 blk_size;

	/* the next 4 entries are guarded by VIRTIO_BLK_F_TOPOLOGY  */
	/* exponent for physical block per logical block. */
	u8 physical_block_exp;
	/* alignment offset in logical blocks. */
	u8 alignment_offset;
	/* minimum I/O size without performance penalty in logical blocks. */
	u16 min_io_size;
	/* optimal sustained I/O size in logical blocks. */
	u32 opt_io_size;

	/* writeback mode (if VIRTIO_BLK_F_CONFIG_WCE) */
	u8 wce;
	u8 unused;

	/* number of vqs, only available when VIRTIO_BLK_F_MQ is set */
	u16 num_queues;

	/* the next 3 entries are guarded by VIRTIO_BLK_F_DISCARD */
	/*
	 * The maximum discard sectors (in 512-byte sectors) for
	 * one segment.
	 */
	u32 max_discard_sectors;
	/*
	 * The maximum number of discard segments in a
	 * discard command.
	 */
	u32 max_discard_seg;
	/* Discard commands must be aligned to this number of sectors. */
	u32 discard_sector_alignment;

	/* the next 3 entries are guarded by VIRTIO_BLK_F_WRITE_ZEROES */
	/*
	 * The maximum number of write zeroes sectors (in 512-byte sectors) in
	 * one segment.
	 */
	u32 max_write_zeroes_sectors;
	/*
	 * The maximum number of segments in a write zeroes
	 * command.
	 */
	u32 max_write_zeroes_seg;
	/*
	 * Set if a VIRTIO_BLK_T_WRITE_ZEROES request may result in the
	 * deallocation of one or more of the sectors.
	 */
	u8 write_zeroes_may_unmap;

	u8 unused1[3];

	/* the next 3 entries are guarded by VIRTIO_BLK_F_SECURE_ERASE */
	/*
	 * The maximum secure erase sectors (in 512-byte sectors) for
	 * one segment.
	 */
	u32 max_secure_erase_sectors;
	/*
	 * The maximum number of secure erase segments in a
	 * secure erase command.
	 */
	u32 max_secure_erase_seg;
	/* Secure erase commands must be aligned to this number of sectors. */
	u32 secure_erase_sector_alignment;

	/* Zoned block device characteristics (if VIRTIO_BLK_F_ZONED) */
	struct virtio_blk_zoned_characteristics {
		u32 zone_sectors;
		u32 max_open_zones;
		u32 max_active_zones;
		u32 max_append_sectors;
		u32 write_granularity;
		u8 model;
		u8 unused2[3];
	} zoned;
} __attribute__((packed));

#define VIRTIO_BLK_SECTOR_SIZE 512
#define VIRTIO_BLK_DEFAULT_QUEUE_SIZE 16

struct virtio_blk_io_desc {
	u64 buf_phys;
	u64 sector;
	usize_t sec_count;
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
int virtio_blk_enable_irq(u32 hart_id);
int virtio_blk_mknod(void);

#endif
