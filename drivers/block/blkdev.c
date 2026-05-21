#include <brk/blkdev.h>
#include <brk/device.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/types.h>

static struct dev_map bd_map;
static struct hlist_head bd_htable[MAJOR_MAX];
static SPINLOCK_DEFINE(bd_htable_lock);

void blkdev_registry_init(void)
{
	dev_map_init(&bd_map, FIRST_DYNAMIC_BLKDEV_MAJOR);
}

int blkdev_register(struct blkdev *bd)
{
	dev_t dev = bd->dev;

	if (!bd || !IS_BLKDEV(dev))
		return -EINVAL;
	if (!minor_is_allocated(&bd_map, MAJOR(dev), MINOR(dev)))
		return -EINVAL;

	klog_debug("%s: devtype=%u, major=%u, minor=%u\n", __func__,
		   DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spinlock_acquire(&bd_htable_lock);
	hlist_add_head(&bd->hlist, &bd_htable[MAJOR(dev)]);
	spinlock_release(&bd_htable_lock);

	return 0;
}

static struct blkdev *blkdev_get_no_lock(dev_t dev)
{
	struct blkdev *bd;

	hlist_for_each_entry(bd, &bd_htable[MAJOR(dev)], hlist) {
		if (bd->dev == dev)
			return bd;
	}

	return NULL;
}

void blkdev_unregister(struct blkdev *bd)
{
	spinlock_acquire(&bd_htable_lock);
	hlist_del_init(&bd->hlist);
	spinlock_release(&bd_htable_lock);
}

struct blkdev *blkdev_get(dev_t dev)
{
	struct blkdev *bd;

	klog_debug("%s: devtype=%u, major=%u, minor=%u\n", __func__,
		   DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spinlock_acquire(&bd_htable_lock);
	bd = blkdev_get_no_lock(dev);
	spinlock_release(&bd_htable_lock);
	return bd;
}

int blkdev_alloc_major(unsigned *major_out)
{
	return alloc_dev_major(&bd_map, major_out);
}

void blkdev_free_major(unsigned major)
{
	free_dev_major(&bd_map, major);
}

int blkdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	return alloc_dev_minor(&bd_map, major, minor_out);
}

void blkdev_free_minor(unsigned major, unsigned minor)
{
	free_dev_minor(&bd_map, major, minor);
}

int blkdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out)
{
	return alloc_dev_region(&bd_map, major, base_minor, count, dev_out);
}

void blkdev_free_region(unsigned major, unsigned minor, unsigned count)
{
	free_dev_region(&bd_map, major, minor, count);
}

int blkdev_check_bounds(struct blkdev *bd, u64 blk_id, u32 blk_cnt)
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

static int bdev_readpage(struct page_cache *m, struct cached_page *cp)
{
	struct blkdev *bd = m->host;
	u32 bs = bd->phy_bsize;
	u32 nsec;
	u64 sector;
	int err;

	if (bs == 0 || PAGE_SIZE % bs != 0)
		return -EIO;
	if (cp->page == NULL)
		return -EIO;

	nsec = (u32)(PAGE_SIZE / bs);
	sector = (u64)cp->index * nsec;

	err = blkdev_check_bounds(bd, sector, nsec);
	if (err)
		return err;

	return bd->ops.read(bd, sector, (void *)page_to_virt(cp->page), nsec);
}

static int bdev_writepage(struct page_cache *m, struct cached_page *cp)
{
	struct blkdev *bd = m->host;
	u32 bs = bd->phy_bsize;
	u32 nsec;
	u64 sector;
	int err;

	if (bs == 0 || PAGE_SIZE % bs != 0)
		return -EIO;

	nsec = (u32)(PAGE_SIZE / bs);
	sector = (u64)cp->index * nsec;

	err = blkdev_check_bounds(bd, sector, nsec);
	if (err)
		return err;

	return bd->ops.write(bd, sector, (const void *)page_to_virt(cp->page),
			     nsec);
}

static const struct page_cache_ops bdev_aops = {
	.readpage = bdev_readpage,
	.writepage = bdev_writepage,
};

int bdev_read_page(struct blkdev *bd, u64 index, void *buf)
{
	struct cached_page *cp;

	if (!bd->bd_mapping)
		return -EIO;

	cp = read_mapping_page(bd->bd_mapping, (pgoff_t)index);
	if (IS_ERR(cp))
		return PTR_ERR(cp);

	memcpy(buf, cached_page_addr(cp), PAGE_SIZE);
	cached_page_put(cp);
	return 0;
}

int bdev_write_page(struct blkdev *bd, u64 index, const void *buf)
{
	struct cached_page *cp;
	int err;

	if (!bd->bd_mapping)
		return -EIO;

	cp = find_or_create_page(bd->bd_mapping, (pgoff_t)index);
	if (!cp)
		return -ENOMEM;

	cached_page_lock(cp);
	memcpy(cached_page_addr(cp), buf, PAGE_SIZE);
	cached_page_mark_uptodate(cp);
	err = bd->bd_mapping->a_ops->writepage(bd->bd_mapping, cp);
	cached_page_unlock(cp);
	cached_page_put(cp);
	return err;
}

struct blkdev *blkdev_alloc(void)
{
	struct blkdev *bd = kzalloc(sizeof(*bd));
	if (!bd)
		return NULL;

	bd->bd_mapping = address_space_alloc(bd, &bdev_aops);
	if (!bd->bd_mapping) {
		kfree(bd);
		return NULL;
	}
	return bd;
}

void blkdev_free(struct blkdev *bd)
{
	address_space_free(bd->bd_mapping);
	bd->bd_mapping = NULL;
	kfree(bd);
}

static int blkdev_open(struct fs_inode *inode, struct opened_file *file)
{
	struct blkdev *bd;

	bd = blkdev_get(inode->i_rdev);
	if (!bd)
		return -ENODEV;

	file->f_op = &blkdev_fops;
	file->private_data = bd;
	return 0;
}

static ssize_t blkdev_read(struct opened_file *file, char *buf, usize_t size,
			   loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EOPNOTSUPP;
}

static ssize_t blkdev_write(struct opened_file *file, const char *buf,
			    usize_t size, loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EOPNOTSUPP;
}

static loff_t blkdev_llseek(struct opened_file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -EOPNOTSUPP;
}

static int blkdev_iterate_shared(struct opened_file *file,
				 struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -EOPNOTSUPP;
}

static int blkdev_fsync(struct opened_file *file, loff_t start, loff_t end,
			int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return -EOPNOTSUPP;
}

static int blkdev_flush(struct opened_file *file)
{
	(void)file;
	return -EOPNOTSUPP;
}

static long blkdev_ioctl(struct opened_file *file, unsigned int cmd,
			 unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

const struct opened_file_ops blkdev_fops = {
	.open = blkdev_open,
	.read = blkdev_read,
	.write = blkdev_write,
	.llseek = blkdev_llseek,
	.iterate_shared = blkdev_iterate_shared,
	.fsync = blkdev_fsync,
	.flush = blkdev_flush,
	.ioctl = blkdev_ioctl,
};
