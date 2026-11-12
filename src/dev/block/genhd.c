#include <brk/blkdev.h>
#include <brk/device.h>
#include <brk/genhd.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <inttypes.h>
#include <uapi/errno.h>
#include <uapi/stat.h>

#include "genhd_internal.h"

static LIST_DEFINE(disk_list);
static SPINLOCK_DEFINE(disk_list_lock);

void gendisk_name(const struct gendisk *disk, unsigned int partno, char *buf,
		  size_t len)
{
	if (partno == 0)
		snprintf(buf, len, "%s", disk->disk_name);
	else
		snprintf(buf, len, "%sp%u", disk->disk_name, partno);
}

struct gendisk *gendisk_alloc(unsigned int major, unsigned int first_minor,
			      unsigned int minors, const char *name,
			      u64 nr_sects, struct device *parent, void *priv)
{
	struct gendisk *disk;
	struct block_dev *bdev;

	if (!name || !minors || nr_sects == 0)
		return NULL;
	if ((u64)first_minor + minors > MINOR_MAX)
		return NULL;

	disk = kzalloc(sizeof(*disk));
	if (!disk)
		return NULL;

	bdev = blkdev_alloc();
	if (!bdev) {
		kfree(disk);
		return NULL;
	}

	disk->major = major;
	disk->first_minor = first_minor;
	disk->minors = minors;
	strlcpy(disk->disk_name, name, sizeof(disk->disk_name));
	disk->nr_sects = nr_sects;
	disk->priv = priv;
	disk->bdev = bdev;

	device_init(&disk->dev, disk->disk_name);
	disk->dev.parent = parent;
	bdev->dev = &disk->dev;

	bdev->devt = MKBLKDEV(major, first_minor);
	bdev->phy_bsize = BLOCK_SECTOR_SIZE;
	bdev->phy_bcnt = nr_sects;

	return disk;
}

void gendisk_free(struct gendisk *disk)
{
	if (!disk)
		return;

	if (disk->bdev)
		disk->bdev->bd_queue = NULL;
	blk_cleanup_queue(disk->queue);
	disk->queue = NULL;

	if (disk->bdev) {
		blkdev_free(disk->bdev);
		disk->bdev = NULL;
	}
	kfree(disk);
}

int add_disk(struct gendisk *disk)
{
	int err;

	if (!disk || !disk->bdev || !disk->queue)
		return -EINVAL;

	err = blkdev_register(disk->bdev);
	if (err)
		return err;

	disk->dev.devt = disk->bdev->devt;
	disk->dev.mode = S_IFBLK | 0666;
	err = device_register(&disk->dev);
	if (err)
		pr_warn("genhd: cannot register %s: %d\n", disk->disk_name,
			err);

	pr_info("genhd: %s: %u minors, %" PRIu64 " sectors\n", disk->disk_name,
		disk->minors, disk->nr_sects);

	spin_lock(&disk_list_lock);
	list_add_tail(&disk->list, &disk_list);
	spin_unlock(&disk_list_lock);
	return 0;
}

void gendisk_scan_partitions(void)
{
	struct gendisk **disks;
	struct gendisk *disk;
	size_t count = 0;
	size_t i = 0;
	irqstate_t flags;

	flags = spin_lock_irqsave(&disk_list_lock);
	list_for_each_entry(disk, &disk_list, list)
		if (!disk->scanned)
			count++;
	if (!count) {
		spin_unlock_irqrestore(&disk_list_lock, flags);
		return;
	}

	disks = kcalloc(count, sizeof(*disks));
	if (!disks) {
		spin_unlock_irqrestore(&disk_list_lock, flags);
		return;
	}

	list_for_each_entry(disk, &disk_list, list) {
		if (!disk->scanned) {
			disk->scanned = true;
			disks[i++] = disk;
		}
	}
	spin_unlock_irqrestore(&disk_list_lock, flags);

	for (size_t j = 0; j < i; ++j)
		scan_partitions(disks[j]);
	kfree(disks);
}

void del_gendisk(struct gendisk *disk)
{
	if (!disk)
		return;

	spin_lock(&disk_list_lock);
	if (!list_empty(&disk->list))
		list_del_init(&disk->list);
	spin_unlock(&disk_list_lock);

	for (unsigned int i = 0; i < GENHD_MAX_PARTITIONS; ++i) {
		struct hd_struct *part = &disk->parts[i];
		struct block_dev *bdev = part->bdev;

		if (!bdev)
			continue;
		device_unregister(&part->dev);
		blkdev_unregister(bdev);
		blkdev_free(bdev);
		part->bdev = NULL;
	}

	if (disk->bdev) {
		device_unregister(&disk->dev);
		blkdev_unregister(disk->bdev);
	}
	gendisk_free(disk);
}
