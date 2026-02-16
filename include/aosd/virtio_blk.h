#ifndef AOSD_VIRTIO_BLK_H
#define AOSD_VIRTIO_BLK_H

#include <aosd/types.h>
#include <aosd/virtio.h>

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

#define SECTOR_SIZE 512

struct virtio_blk_req {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

struct virtio_blk_transation {
	void *buf;
	uint64_t sector;
	size_t sec_count;
	bool is_write;
	bool completed;
};

struct virtio_blk_track {
	struct virtio_blk_transation *trans;
	char status;
};

int virtio_blk_init(uint32_t hart_id, struct virtio_device *dev,
		    unsigned int queue_size);
int virtio_blk_read(uint64_t sector, void *buf, size_t sec_count);
int virtio_blk_write(uint64_t sector, const void *buf, size_t sec_count);

#endif
