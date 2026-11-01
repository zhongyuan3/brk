#ifndef BRK_GENHD_H
#define BRK_GENHD_H

#include <brk/bio.h>
#include <brk/blkdev.h>
#include <brk/device_model.h>
#include <brk/types.h>

/*
 * Generic disk layer.
 *
 * A gendisk describes one physical disk: its device number range, size and
 * request queue.  add_disk() creates the whole-disk block_dev and, if a
 * DOS/MBR partition table is present, one block_dev per primary partition.
 * Each block_dev is an independently addressable view (whole disk or
 * partition) with its own page cache; I/O is routed through the disk's
 * request queue and eventually the driver's queue_rq() callback.
 */
#define GENHD_NAME_LEN 32
#define GENHD_MAX_PARTITIONS 4
#define GENHD_MINORS (GENHD_MAX_PARTITIONS + 1)

struct hd_struct {
	u64 start_sect;
	u64 nr_sects;
	unsigned int partno;
	struct block_dev *bdev;
	struct device dev;
};

struct gendisk {
	unsigned int major;
	unsigned int first_minor;
	unsigned int minors;
	char disk_name[GENHD_NAME_LEN];
	u64 nr_sects;
	void *priv;
	struct block_dev *bdev;
	struct request_queue *queue;
	struct device dev;
	struct hd_struct parts[GENHD_MAX_PARTITIONS];
	struct list_head list;
	bool scanned;
};

struct gendisk *gendisk_alloc(unsigned int major, unsigned int first_minor,
			      unsigned int minors, const char *name,
			      u64 nr_sects, struct device *parent, void *priv);
void gendisk_free(struct gendisk *disk);

int add_disk(struct gendisk *disk);
void del_gendisk(struct gendisk *disk);

/*
 * Scan every registered disk for an MBR partition table and create the
 * partition block devices and devtmpfs nodes.  Must run in sleepable task context with
 * interrupts enabled, since probing issues real I/O.
 */
void gendisk_scan_partitions(void);

/* "virtio_blk0" for partno 0, "virtio_blk0p1" for partno 1. */
void gendisk_name(const struct gendisk *disk, unsigned int partno, char *buf,
		  size_t len);

static inline dev_t gendisk_devt(const struct gendisk *disk)
{
	return MKBLKDEV(disk->major, disk->first_minor);
}

#endif
