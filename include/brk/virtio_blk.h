#ifndef BRK_VIRTIO_BLK_H
#define BRK_VIRTIO_BLK_H

#include <brk/types.h>
#include <brk/virtio.h>

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

#define SECTOR_SIZE 512

struct virtio_blk_req {
	u32 type;
	u32 reserved;
	u64 sector;
};

struct virtio_blk_transaction {
	u64 buf_phys;
	u64 sector;
	usize_t sec_count;
	bool is_write;
	bool completed;
};

struct virtio_blk_track {
	struct virtio_blk_transaction *trans;
	char status;
};

int virtio_blk_init(struct virtio_device *dev, unsigned int queue_size);
void virtio_blk_init_hart(u32 hart_id);
int virtio_blk_read(u64 sector, u64 buf_phys, usize_t sec_count);
int virtio_blk_write(u64 sector, u64 buf_phys, usize_t sec_count);
void virtio_blk_intr(void);

#endif
