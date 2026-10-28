#include <arch/asm/page.h>
#include <brk/assert.h>
#include <brk/blkdev.h>
#include <brk/device.h>
#include <brk/devno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/initcall.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/types.h>

static struct devno_allocator bd_devno;
static struct hlist_head bd_htable[MAJOR_MAX];
static SPINLOCK_DEFINE(bd_htable_lock);

void blkdev_registry_init(void)
{
	devno_allocator_init(&bd_devno, FIRST_DYNAMIC_BLKDEV_MAJOR);
}
core_initcall(blkdev_registry_init);

int blkdev_register(struct block_dev *bd)
{
	dev_t dev;

	if (!bd)
		return -EINVAL;
	dev = bd->dev;
	if (!IS_BLKDEV(dev))
		return -EINVAL;
	if (!devno_minor_is_allocated(&bd_devno, MAJOR(dev), MINOR(dev)))
		return -EINVAL;

	pr_debug("%s: devtype=%" PRIu32 ", major=%u, minor=%u\n", __func__,
		   DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spin_lock(&bd_htable_lock);
	if (!hlist_unhashed(&bd->hlist)) {
		spin_unlock(&bd_htable_lock);
		return -EBUSY;
	}
	bd->dead = false;
	hlist_add_head(&bd->hlist, &bd_htable[MAJOR(dev)]);
	spin_unlock(&bd_htable_lock);

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
	if (!bd)
		return;

	spin_lock(&bd_htable_lock);
	bd->dead = true;
	if (!hlist_unhashed(&bd->hlist))
		hlist_del_init(&bd->hlist);
	spin_unlock(&bd_htable_lock);
}

struct block_dev *blkdev_get(dev_t dev)
{
	struct block_dev *bd;

	pr_debug("%s: devtype=%" PRIu32 ", major=%u, minor=%u\n", __func__,
		   DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spin_lock(&bd_htable_lock);
	bd = blkdev_get_no_lock(dev);
	if (bd && (bd->dead || !refcnt_inc_unless_zero(&bd->ref)))
		bd = NULL;
	spin_unlock(&bd_htable_lock);
	return bd;
}

void blkdev_put(struct block_dev *bd)
{
	if (!bd)
		return;
	if (refcnt_dec_fetch(&bd->ref) > 0)
		return;

	ASSERT(hlist_unhashed(&bd->hlist));
	page_cache_destroy(bd->bd_mapping);
	bd->bd_mapping = NULL;
	kfree(bd);
}

int blkdev_alloc_major(unsigned *major_out)
{
	return devno_alloc_major(&bd_devno, major_out);
}

void blkdev_free_major(unsigned major)
{
	devno_free_major(&bd_devno, major);
}

int blkdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	return devno_alloc_minor(&bd_devno, major, minor_out);
}

void blkdev_free_minor(unsigned major, unsigned minor)
{
	devno_free_minor(&bd_devno, major, minor);
}

int blkdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out)
{
	return devno_alloc_region(&bd_devno, major, base_minor, count,
				  dev_out);
}

void blkdev_free_region(unsigned major, unsigned minor, unsigned count)
{
	devno_free_region(&bd_devno, major, minor, count);
}

int blkdev_check_bounds(struct block_dev *bd, u64 blk_id, u32 blk_cnt)
{
	if (blk_cnt == 0)
		return 0;
	if (blk_id >= bd->phy_bcnt) {
		pr_warn("%s(): invalid blk_id %" PRIu64 ", phy_bcnt %" PRIu64
			  "\n",
			  __func__, blk_id, bd->phy_bcnt);
		return -ENXIO;
	}
	if (bd->phy_bcnt - blk_id < blk_cnt) {
		pr_warn("%s(): invalid blk_cnt %" PRIu32 ", phy_bcnt %" PRIu64
			  "\n",
			  __func__, blk_cnt, bd->phy_bcnt);
		return -ENXIO;
	}
	return 0;
}

int blkdev_read(struct block_dev *bd, u64 blk_id, void *buf, u32 blk_cnt)
{
	int err;

	if (!bd->ops.read)
		return -EOPNOTSUPP;

	err = blkdev_check_bounds(bd, blk_id, blk_cnt);
	if (err)
		return err;

	return bd->ops.read(bd, blk_id, buf, blk_cnt);
}

int blkdev_write(struct block_dev *bd, u64 blk_id, const void *buf, u32 blk_cnt)
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
	u32 bs;
	u32 nsec;
	u64 sector;
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
	u32 bs;
	u32 nsec;
	u64 sector;
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
	refcnt_init(&bd->ref, 1);
	hlist_node_init(&bd->hlist);
	bd->dead = false;
	return bd;
}

void blkdev_free(struct block_dev *bd)
{
	blkdev_put(bd);
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

static int blkdev_file_release(struct fs_inode *inode, struct fs_file *file)
{
	struct block_dev *bd = file->private_data;

	(void)inode;
	file->private_data = NULL;
	blkdev_put(bd);
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
	.release = blkdev_file_release,
	.read = blkdev_file_read,
	.write = blkdev_file_write,
	.llseek = blkdev_file_llseek,
	.iterate_shared = blkdev_file_iterate_shared,
	.fsync = blkdev_file_fsync,
	.flush = blkdev_file_flush,
	.ioctl = blkdev_file_ioctl,
};
