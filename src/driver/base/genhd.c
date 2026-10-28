#include <brk/blkdev.h>
#include <brk/devnode.h>
#include <brk/genhd.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/spinlock.h>
#include <brk/printk.h>
#include <brk/printf.h>
#include <brk/refcnt.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/stat.h>

#define MBR_SIGNATURE 0xAA55u
#define MBR_PART_COUNT 4
#define MBR_SECTOR_SIZE 512
/* GPT protective MBR; a real GPT partition table is not parsed yet. */
#define MBR_TYPE_GPT_PROTECTIVE 0xEE

struct mbr_partition {
	u8 status;
	u8 chs_first[3];
	u8 type;
	u8 chs_last[3];
	u32 start_lba;
	u32 nr_sectors;
} __attribute__((packed));

struct mbr_sector {
	u8 bootstrap[446];
	struct mbr_partition parts[MBR_PART_COUNT];
	u16 signature;
} __attribute__((packed));

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
			      u64 nr_sects, void *priv)
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

	bdev->dev = MKBLKDEV(major, first_minor);
	bdev->phy_bsize = MBR_SECTOR_SIZE;
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

static int add_partition(struct gendisk *disk, unsigned int partno, u64 start,
			 u64 nr_sects)
{
	struct block_dev *bdev;
	struct hd_struct *part;
	char path[GENHD_NAME_LEN + 16];
	char name[GENHD_NAME_LEN + 8];
	int err;

	if (!partno || partno > GENHD_MAX_PARTITIONS || partno >= disk->minors)
		return -EINVAL;

	part = &disk->parts[partno - 1];
	if (part->bdev)
		return -EBUSY;
	if (!start || start >= disk->nr_sects ||
	    nr_sects > disk->nr_sects - start)
		return -EINVAL;

	bdev = blkdev_alloc();
	if (!bdev)
		return -ENOMEM;

	bdev->dev = MKBLKDEV(disk->major, disk->first_minor + partno);
	bdev->phy_bsize = disk->bdev->phy_bsize;
	bdev->phy_bcnt = nr_sects;
	bdev->bd_parent = disk->bdev;
	bdev->bd_start_sect = start;
	/* Keep the parent alive for as long as the partition view exists. */
	refcnt_inc(&disk->bdev->ref);

	err = blkdev_register(bdev);
	if (err) {
		blkdev_put(bdev);
		return err;
	}

	part->start_sect = start;
	part->nr_sects = nr_sects;
	part->partno = partno;
	part->bdev = bdev;

	gendisk_name(disk, partno, name, sizeof(name));
	snprintf(path, sizeof(path), "/dev/%s", name);
	err = devnode_register(path, bdev->dev, S_IFBLK | 0666);
	if (err)
		pr_warn("genhd: cannot register %s: %d\n", path, err);

	pr_info("genhd: %s: partition %u, start %" PRIu64
		", sectors %" PRIu64 "\n",
		name, partno, start, nr_sects);
	return 0;
}

static void scan_partitions(struct gendisk *disk)
{
	struct mbr_sector *mbr;
	u8 *buf;

	if (disk->minors <= 1)
		return;

	buf = kmalloc(MBR_SECTOR_SIZE);
	if (!buf) {
		pr_warn("genhd: %s: no memory for partition scan\n",
			disk->disk_name);
		return;
	}

	if (blkdev_read(disk->bdev, 0, buf, 1)) {
		kfree(buf);
		return;
	}
	mbr = (struct mbr_sector *)buf;

	if (mbr->signature != MBR_SIGNATURE) {
		kfree(buf);
		return;
	}

	for (unsigned int i = 0; i < MBR_PART_COUNT; ++i) {
		struct mbr_partition *p = &mbr->parts[i];
		u64 start = p->start_lba;
		u64 nr_sects = p->nr_sectors;
		int err;

		if (!p->type || !nr_sects)
			continue;

		if (p->type == MBR_TYPE_GPT_PROTECTIVE) {
			pr_info("genhd: %s: GPT partition table is not supported\n",
				disk->disk_name);
			break;
		}

		if (i + 1 >= disk->minors)
			break;

		err = add_partition(disk, i + 1, start, nr_sects);
		if (err)
			pr_warn("genhd: %s: cannot add partition %u: %d\n",
				disk->disk_name, i + 1, err);
	}

	kfree(buf);
}

int add_disk(struct gendisk *disk)
{
	char path[GENHD_NAME_LEN + 16];
	int err;

	if (!disk || !disk->bdev || !disk->queue)
		return -EINVAL;

	err = blkdev_register(disk->bdev);
	if (err)
		return err;

	snprintf(path, sizeof(path), "/dev/%s", disk->disk_name);
	err = devnode_register(path, disk->bdev->dev, S_IFBLK | 0666);
	if (err)
		pr_warn("genhd: cannot register %s: %d\n", path, err);

	pr_info("genhd: %s: %u minors, %" PRIu64 " sectors\n",
		disk->disk_name, disk->minors, disk->nr_sects);

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
	char path[GENHD_NAME_LEN + 16];

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
		gendisk_name(disk, part->partno, path, sizeof(path));
		devnode_unregister(path);
		blkdev_unregister(bdev);
		blkdev_free(bdev);
		part->bdev = NULL;
	}

	if (disk->bdev) {
		snprintf(path, sizeof(path), "/dev/%s", disk->disk_name);
		devnode_unregister(path);
		blkdev_unregister(disk->bdev);
	}
	gendisk_free(disk);
}
