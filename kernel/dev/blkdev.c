#include <brk/dev.h>
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

static struct dev_registry blkdev_registry;

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

static int bdev_readpage(struct address_space *m, struct cached_page *cp)
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

static int bdev_writepage(struct address_space *m, struct cached_page *cp)
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

static const struct address_space_operations bdev_aops = {
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

int blkdev_register(struct blkdev *bd, dev_t dev)
{
	if (!IS_BLKDEV(dev))
		return -EINVAL;
	return dev_registry_register(&blkdev_registry, (struct dev_slot *)bd,
				     dev, BLKDEV);
}

void blkdev_unregister(struct blkdev *bd)
{
	dev_registry_unregister(&blkdev_registry, (struct dev_slot *)bd);
}

struct blkdev *blkdev_get(dev_t dev)
{
	return (struct blkdev *)dev_registry_get(&blkdev_registry, dev);
}

int blkdev_alloc_major(unsigned *major_out)
{
	return dev_registry_alloc_major(&blkdev_registry, major_out);
}

void blkdev_free_major(unsigned major)
{
	dev_registry_free_major(&blkdev_registry, major);
}

int blkdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	return dev_registry_alloc_minor(&blkdev_registry, major, minor_out,
					BLKDEV);
}

int blkdev_alloc_devnum(dev_t *dev_out)
{
	return dev_registry_alloc_devnum(&blkdev_registry, dev_out, BLKDEV);
}

void blkdev_registry_init(void)
{
	dev_registry_init(&blkdev_registry);
}

static int blkdev_open(struct inode *inode, struct file *file)
{
	(void)inode;
	file->f_op = &blkdev_fops;
	return 0;
}

static ssize_t blkdev_read(struct file *file, char *buf, usize_t size,
			   loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EOPNOTSUPP;
}

static ssize_t blkdev_write(struct file *file, const char *buf, usize_t size,
			    loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EOPNOTSUPP;
}

static loff_t blkdev_llseek(struct file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return 0;
}

static int blkdev_iterate_shared(struct file *file, struct dir_context *ctx)
{
	(void)file;
	(void)ctx;
	return 0;
}

static int blkdev_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int blkdev_flush(struct file *file)
{
	(void)file;
	return 0;
}

static long blkdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

const struct file_operations blkdev_fops = {
	.open = blkdev_open,
	.read = blkdev_read,
	.write = blkdev_write,
	.llseek = blkdev_llseek,
	.iterate_shared = blkdev_iterate_shared,
	.fsync = blkdev_fsync,
	.flush = blkdev_flush,
	.ioctl = blkdev_ioctl,
};
