#include <arch/page.h>
#include <brk/bitmap.h>
#include <brk/blkdev.h>
#include <brk/device.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/types.h>

struct block_dev_region {
	struct hlist_node entry;
	unsigned minor_start;
	unsigned minor_count;
};

static struct hlist_head bd_regs[MAJOR_MAX];
static BITMAP_DECLARE(bd_major_pooled, MAJOR_MAX);
static SPINLOCK_DEFINE(bd_lock);
static struct hlist_head bd_htable[MAJOR_MAX];
static SPINLOCK_DEFINE(bd_htable_lock);

static int blkdev_alloc_major_no_lock(unsigned base_major, unsigned *major_out)
{
	size_t bit = 0;

	if (bitmap_alloc_bit_from(bd_major_pooled, MAJOR_MAX, base_major,
				  &bit)) {
		*major_out = bit;
		return 0;
	}

	return -ENOMEM;
}

static void blkdev_free_major_no_lock(unsigned major)
{
	bitmap_free_bit(bd_major_pooled, MAJOR_MAX, major);
}

static int blkdev_alloc_region_no_lock(unsigned major, unsigned base_minor,
				       unsigned count, dev_t *dev_out)
{
	struct block_dev_region *curr, *new_entry;
	int err = 0;
	bool major_allocated = false;
	unsigned minor = base_minor;
	struct block_dev_region *last = NULL;

	if (major == 0) {
		err = blkdev_alloc_major_no_lock(0, &major);
		if (err)
			return err;
		major_allocated = true;
	} else {
		if (!bitmap_test_bit(bd_major_pooled, major))
			return -EINVAL;
	}

	hlist_for_each_entry(curr, &bd_regs[major], entry) {
		if (curr->minor_start > minor &&
		    curr->minor_start - minor >= count) {
			new_entry = kzalloc(sizeof(*new_entry));
			if (!new_entry) {
				if (major_allocated)
					blkdev_free_major_no_lock(major);
				return -ENOMEM;
			}
			new_entry->minor_start = minor;
			new_entry->minor_count = count;
			hlist_add_before(&new_entry->entry, &curr->entry);
			*dev_out = MKDEV(major, minor);
			return 0;
		}
		minor = curr->minor_start + curr->minor_count;
		last = curr;
	}

	new_entry = kzalloc(sizeof(*new_entry));
	if (!new_entry) {
		if (major_allocated)
			blkdev_free_major_no_lock(major);
		return -ENOMEM;
	}
	new_entry->minor_start = minor;
	new_entry->minor_count = count;
	if (last)
		hlist_add_behind(&new_entry->entry, &last->entry);
	else
		hlist_add_head(&new_entry->entry, &bd_regs[major]);
	*dev_out = MKDEV(major, minor);
	return 0;
}

static bool blkdev_minor_is_allocated(unsigned major, unsigned minor)
{
	struct block_dev_region *curr;

	if (!bitmap_test_bit(bd_major_pooled, major))
		return false;

	hlist_for_each_entry(curr, &bd_regs[major], entry) {
		if (curr->minor_start <= minor &&
		    curr->minor_start + curr->minor_count > minor)
			return true;
	}
	return false;
}

void blkdev_registry_init(void)
{
	bitmap_fill(bd_major_pooled, FIRST_DYNAMIC_BLKDEV_MAJOR);
}

int blkdev_register(struct block_dev *bd)
{
	dev_t dev = bd->dev;

	if (!bd || !IS_BLKDEV(dev))
		return -EINVAL;
	spinlock_acquire(&bd_lock);
	if (!blkdev_minor_is_allocated(MAJOR(dev), MINOR(dev))) {
		spinlock_release(&bd_lock);
		return -EINVAL;
	}
	spinlock_release(&bd_lock);

	klog_debug("%s: devtype=%u, major=%u, minor=%u\n", __func__,
		   DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spinlock_acquire(&bd_htable_lock);
	hlist_add_head(&bd->hlist, &bd_htable[MAJOR(dev)]);
	spinlock_release(&bd_htable_lock);

	return 0;
}

static struct block_dev *blkdev_get_no_lock(dev_t dev)
{
	struct block_dev *bd;

	hlist_for_each_entry(bd, &bd_htable[MAJOR(dev)], hlist) {
		if (bd->dev == dev)
			return bd;
	}

	return NULL;
}

void blkdev_unregister(struct block_dev *bd)
{
	spinlock_acquire(&bd_htable_lock);
	hlist_del_init(&bd->hlist);
	spinlock_release(&bd_htable_lock);
}

struct block_dev *blkdev_get(dev_t dev)
{
	struct block_dev *bd;

	klog_debug("%s: devtype=%u, major=%u, minor=%u\n", __func__,
		   DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spinlock_acquire(&bd_htable_lock);
	bd = blkdev_get_no_lock(dev);
	spinlock_release(&bd_htable_lock);
	return bd;
}

int blkdev_alloc_major(unsigned *major_out)
{
	int err;

	if (!major_out)
		return -EINVAL;

	spinlock_acquire(&bd_lock);
	err = blkdev_alloc_major_no_lock(0, major_out);
	spinlock_release(&bd_lock);
	return err;
}

void blkdev_free_major(unsigned major)
{
	if (major >= MAJOR_MAX)
		return;

	spinlock_acquire(&bd_lock);
	blkdev_free_major_no_lock(major);
	spinlock_release(&bd_lock);
}

int blkdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	dev_t dev = 0;
	int err;

	if (major >= MAJOR_MAX || !minor_out)
		return -EINVAL;

	spinlock_acquire(&bd_lock);
	if (!bitmap_test_bit(bd_major_pooled, major)) {
		spinlock_release(&bd_lock);
		return -EINVAL;
	}
	err = blkdev_alloc_region_no_lock(major, 0, 1, &dev);
	spinlock_release(&bd_lock);

	if (err)
		return err;
	*minor_out = MINOR(dev);
	return 0;
}

void blkdev_free_minor(unsigned major, unsigned minor)
{
	blkdev_free_region(major, minor, 1);
}

int blkdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out)
{
	int ret;

	if (major >= MAJOR_MAX || count == 0 || !dev_out)
		return -EINVAL;

	spinlock_acquire(&bd_lock);
	ret = blkdev_alloc_region_no_lock(major, base_minor, count, dev_out);
	spinlock_release(&bd_lock);

	return ret;
}

void blkdev_free_region(unsigned major, unsigned minor, unsigned count)
{
	struct block_dev_region *curr;

	if (major >= MAJOR_MAX || minor + count > MINOR_MAX)
		return;

	spinlock_acquire(&bd_lock);
	hlist_for_each_entry(curr, &bd_regs[major], entry) {
		if (curr->minor_start + curr->minor_count <= minor)
			continue;

		if (curr->minor_start == minor && curr->minor_count == count) {
			hlist_del_init(&curr->entry);
			spinlock_release(&bd_lock);
			kfree(curr);
			return;
		}

		klog_error("%s: region %u+%u does not match %u+%u", __func__,
			   curr->minor_start, curr->minor_count, minor, count);
		spinlock_release(&bd_lock);
		return;
	}
	spinlock_release(&bd_lock);
	klog_error("%s: no region at minor %u count %u", __func__, minor,
		   count);
}

int blkdev_check_bounds(struct block_dev *bd, uint64_t blk_id, uint32_t blk_cnt)
{
	if (blk_cnt == 0)
		return 0;
	if (blk_id >= bd->phy_bcnt) {
		klog_warn("%s(): invalid blk_id %lu, phy_bcnt %lu\n", __func__,
			  blk_id, bd->phy_bcnt);
		return -ENXIO;
	}
	if (bd->phy_bcnt - blk_id < blk_cnt) {
		klog_warn("%s(): invalid blk_cnt %u, phy_bcnt %lu\n", __func__,
			  blk_cnt, bd->phy_bcnt);
		return -ENXIO;
	}
	return 0;
}

int blkdev_read(struct block_dev *bd, uint64_t blk_id, void *buf,
		uint32_t blk_cnt)
{
	int err;

	if (!bd->ops.read)
		return -EOPNOTSUPP;

	err = blkdev_check_bounds(bd, blk_id, blk_cnt);
	if (err)
		return err;

	return bd->ops.read(bd, blk_id, buf, blk_cnt);
}

int blkdev_write(struct block_dev *bd, uint64_t blk_id, const void *buf,
		 uint32_t blk_cnt)
{
	int err;

	if (!bd->ops.write)
		return -EOPNOTSUPP;

	err = blkdev_check_bounds(bd, blk_id, blk_cnt);
	if (err)
		return err;

	return bd->ops.write(bd, blk_id, buf, blk_cnt);
}

static int blkdev_read_page(struct page_cache *m, struct cached_page *cp)
{
	struct block_dev *bd;
	uint32_t bs;
	uint32_t nsec;
	uint64_t sector;
	int err;
	void *buf;

	bd = m->host;
	if (!bd)
		return -EIO;
	bs = bd->phy_bsize;
	if (bs == 0 || bs > PAGE_SIZE || PAGE_SIZE % bs != 0)
		return -EIO;
	if (cp->page == NULL)
		return -EIO;

	nsec = PAGE_SIZE / bs;
	sector = cp->index * nsec;

	err = blkdev_check_bounds(bd, sector, nsec);
	if (err)
		return err;

	buf = (void *)page_to_virt(cp->page);
	return bd->ops.read(bd, sector, buf, nsec);
}

static int blkdev_write_page(struct page_cache *m, struct cached_page *cp)
{
	struct block_dev *bd;
	uint32_t bs;
	uint32_t nsec;
	uint64_t sector;
	int err;
	void *buf;

	bd = m->host;
	if (!bd)
		return -EIO;
	bs = bd->phy_bsize;
	if (bs == 0 || bs > PAGE_SIZE || PAGE_SIZE % bs != 0)
		return -EIO;

	nsec = PAGE_SIZE / bs;
	sector = cp->index * nsec;

	err = blkdev_check_bounds(bd, sector, nsec);
	if (err)
		return err;

	buf = (void *)page_to_virt(cp->page);
	return bd->ops.write(bd, sector, buf, nsec);
}

static const struct page_cache_ops bdev_pc_ops = {
	.read_page = blkdev_read_page,
	.write_page = blkdev_write_page,
};

struct block_dev *blkdev_alloc(void)
{
	struct block_dev *bd = kzalloc(sizeof(*bd));
	if (!bd)
		return NULL;

	bd->bd_mapping = page_cache_create(bd, &bdev_pc_ops);
	if (!bd->bd_mapping) {
		kfree(bd);
		return NULL;
	}
	return bd;
}

void blkdev_free(struct block_dev *bd)
{
	page_cache_destroy(bd->bd_mapping);
	bd->bd_mapping = NULL;
	kfree(bd);
}

static int blkdev_file_open(struct fs_inode *inode, struct fs_file *file)
{
	struct block_dev *bd;

	bd = blkdev_get(inode->rdev);
	if (!bd)
		return -ENODEV;

	file->ops = &blkdev_fops;
	file->private_data = bd;
	return 0;
}

static ssize_t blkdev_file_read(struct fs_file *file, char *buf, size_t size,
				loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EOPNOTSUPP;
}

static ssize_t blkdev_file_write(struct fs_file *file, const char *buf,
				 size_t size, loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EOPNOTSUPP;
}

static loff_t blkdev_file_llseek(struct fs_file *file, loff_t offset,
				 int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -EOPNOTSUPP;
}

static int blkdev_file_iterate_shared(struct fs_file *file,
				      struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -EOPNOTSUPP;
}

static int blkdev_file_fsync(struct fs_file *file, loff_t start, loff_t end,
			     int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return -EOPNOTSUPP;
}

static int blkdev_file_flush(struct fs_file *file)
{
	(void)file;
	return -EOPNOTSUPP;
}

static long blkdev_file_ioctl(struct fs_file *file, unsigned int cmd,
			      unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

const struct fs_file_ops blkdev_fops = {
	.open = blkdev_file_open,
	.read = blkdev_file_read,
	.write = blkdev_file_write,
	.llseek = blkdev_file_llseek,
	.iterate_shared = blkdev_file_iterate_shared,
	.fsync = blkdev_file_fsync,
	.flush = blkdev_file_flush,
	.ioctl = blkdev_file_ioctl,
};
