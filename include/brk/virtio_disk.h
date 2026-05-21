#ifndef BRK_VIRTIO_DISK_H
#define BRK_VIRTIO_DISK_H

#include <brk/lock.h>
#include <brk/types.h>
#include <brk/virtio_blk.h>
#include <brk/virtio_queue.h>

struct virtio_disk_transaction {
	u64 buf_phys;
	u64 sector;
	usize_t sec_count;
	bool is_write;
	bool completed;
};

struct virtio_disk_track {
	struct virtio_disk_transaction *trans;
	char status;
};

struct virtio_disk_device {
	struct virtio_device *vdev;
	struct virtq vq;
	struct virtio_blk_req *reqs;
	struct virtio_disk_track *tracks;
	unsigned queue_size;
	spinlock_t lock;
	struct virtio_blk_config config;
};

struct virtio_disk_driver {
	struct virtio_disk_device **disks;
	struct block_dev **bdevs;
	unsigned num_disks;
	spinlock_t lock;
	unsigned major;
	unsigned minor_start;
};

int virtio_disk_driver_init(void);

struct virtio_disk_device *virtio_disk_device_create(struct virtio_device *dev,
						     unsigned queue_size);
void virtio_disk_device_destroy(struct virtio_disk_device *disk);
int virtio_disk_add_device(struct virtio_disk_device *disk);
void virtio_disk_remove_device(struct virtio_disk_device *disk);
int virtio_disk_enable_irq(u32 hart_id);

int virtio_disk_create_fs_nodes(void);

#endif
