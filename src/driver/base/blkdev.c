#include <arch/asm/page.h>
#include <brk/assert.h>
#include <brk/bio.h>
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
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/ioctl.h>
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
	struct block_dev *parent;

	if (!bd)
		return;
	if (refcnt_dec_fetch(&bd->ref) > 0)
		return;

	ASSERT(hlist_unhashed(&bd->hlist));
	parent = bd->bd_parent;
	page_cache_destroy(bd->bd_mapping);
	bd->bd_mapping = NULL;
	kfree(bd);

	/* A partition view pins the whole-disk block_dev. */
	blkdev_put(parent);
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

static int blkdev_submit_buf(struct block_dev *bd, u64 blk_id, void *buf,
			     size_t len, enum req_op op)
{
	size_t done = 0;
	int err;

	if (len == 0)
		return 0;
	if (bd->phy_bsize != 512 || (len & 511))
		return -EINVAL;

	err = blkdev_check_bounds(bd, blk_id, (u32)(len >> 9));
	if (err)
		return err;

	while (done < len) {
		struct bio bio;
		size_t chunk;
		unsigned int i;

		bio_init(&bio, bd, op);
		bio.bi_sector = blk_id + (done >> 9);

		for (i = 0; i < BIO_MAX_VECS && done < len; ++i) {
			chunk = len - done;
			if (chunk > PAGE_SIZE)
				chunk = PAGE_SIZE;

			err = bio_add_buf(&bio, (u8 *)buf + done, chunk);
			if (err)
				return err;
			done += chunk;
		}

		err = submit_bio_wait(&bio);
		if (err)
			return err;
	}

	return 0;
}

int blkdev_read(struct block_dev *bd, u64 blk_id, void *buf, u32 blk_cnt)
{
	if (!bd)
		return -EINVAL;
	return blkdev_submit_buf(bd, blk_id, buf,
				 (size_t)blk_cnt * bd->phy_bsize,
				 REQ_OP_READ);
}

int blkdev_write(struct block_dev *bd, u64 blk_id, const void *buf, u32 blk_cnt)
{
	if (!bd)
		return -EINVAL;
	return blkdev_submit_buf(bd, blk_id, (void *)buf,
				 (size_t)blk_cnt * bd->phy_bsize,
				 REQ_OP_WRITE);
}

int blkdev_flush(struct block_dev *bd)
{
	struct bio bio;

	if (!bd)
		return -EINVAL;

	bio_init(&bio, bd, REQ_OP_FLUSH);
	return submit_bio_wait(&bio);
}

static int blkdev_read_page(struct page_cache *m, struct cached_page *cp)
{
	struct block_dev *bd;
	u32 bs;
	u32 nsec;
	u32 valid;
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
	buf = (void *)page_to_virt(cp->page);

	if (sector >= bd->phy_bcnt) {
		memset(buf, 0, PAGE_SIZE);
		return 0;
	}

	valid = nsec;
	if (bd->phy_bcnt - sector < valid)
		valid = (u32)(bd->phy_bcnt - sector);

	err = blkdev_check_bounds(bd, sector, valid);
	if (err)
		return err;

	err = blkdev_read(bd, sector, buf, valid);
	if (err)
		return err;

	if (valid < nsec)
		memset((u8 *)buf + valid * bs, 0, PAGE_SIZE - valid * bs);

	return 0;
}

static int blkdev_write_page(struct page_cache *m, struct cached_page *cp)
{
	struct block_dev *bd;
	u32 bs;
	u32 nsec;
	u32 valid;
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

	if (sector >= bd->phy_bcnt)
		return 0;

	valid = nsec;
	if (bd->phy_bcnt - sector < valid)
		valid = (u32)(bd->phy_bcnt - sector);

	err = blkdev_check_bounds(bd, sector, valid);
	if (err)
		return err;

	buf = (void *)page_to_virt(cp->page);
	return blkdev_write(bd, sector, buf, valid);
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
	int err = 0;

	(void)inode;
	file->private_data = NULL;
	if (bd && !bd->dead) {
		err = page_cache_flush(bd->bd_mapping);
		if (!err)
			err = blkdev_flush(bd);
	}
	blkdev_put(bd);
	return err;
}

static ssize_t blkdev_file_read(struct fs_file *file, char *buf, size_t size,
				loff_t *pos)
{
	struct block_dev *bd = file->private_data;
	u64 capacity;
	size_t total = 0;

	if (!bd || bd->dead)
		return -ENODEV;
	if (!(file->mode & FMODE_READ))
		return -EBADF;
	if (*pos < 0)
		return -EINVAL;

	capacity = blkdev_capacity_bytes(bd);
	if ((u64)*pos >= capacity)
		return 0;
	if (size > capacity - (u64)*pos)
		size = (size_t)(capacity - (u64)*pos);

	while (size > 0) {
		pgoff_t index = (pgoff_t)(*pos >> PAGE_SHIFT);
		size_t off = (size_t)(*pos & (PAGE_SIZE - 1));
		size_t n = PAGE_SIZE - off;
		struct cached_page *cp;
		int err;

		if (n > size)
			n = size;

		cp = read_mapping_page(bd->bd_mapping, index);
		if (IS_ERR(cp)) {
			if (total > 0)
				break;
			return PTR_ERR(cp);
		}

		cached_page_lock(cp);
		err = copy_to_user(buf + total,
				   (u8 *)cached_page_addr(cp) + off, n);
		cached_page_unlock(cp);
		cached_page_put(cp);

		if (err)
			return total > 0 ? (ssize_t)total : -EFAULT;

		*pos += (loff_t)n;
		total += n;
		size -= n;
	}

	return (ssize_t)total;
}

static ssize_t blkdev_file_write(struct fs_file *file, const char *buf,
				 size_t size, loff_t *pos)
{
	struct block_dev *bd = file->private_data;
	u64 capacity;
	size_t total = 0;

	if (!bd || bd->dead)
		return -ENODEV;
	if (!(file->mode & FMODE_WRITE))
		return -EBADF;
	if (*pos < 0)
		return -EINVAL;

	capacity = blkdev_capacity_bytes(bd);
	if ((u64)*pos >= capacity)
		return -ENOSPC;
	if (size > capacity - (u64)*pos)
		size = (size_t)(capacity - (u64)*pos);

	while (size > 0) {
		pgoff_t index = (pgoff_t)(*pos >> PAGE_SHIFT);
		size_t off = (size_t)(*pos & (PAGE_SIZE - 1));
		size_t n = PAGE_SIZE - off;
		struct cached_page *cp;
		int err;

		if (n > size)
			n = size;

		/* Partial page writes need the existing surrounding data. */
		cp = read_mapping_page(bd->bd_mapping, index);
		if (IS_ERR(cp)) {
			if (total > 0)
				break;
			return PTR_ERR(cp);
		}

		cached_page_lock(cp);
		err = copy_from_user((u8 *)cached_page_addr(cp) + off,
				     buf + total, n);
		cached_page_unlock(cp);
		if (!err)
			cached_page_mark_dirty(cp);
		cached_page_put(cp);

		if (err)
			return total > 0 ? (ssize_t)total : -EFAULT;

		*pos += (loff_t)n;
		total += n;
		size -= n;
	}

	return (ssize_t)total;
}

static loff_t blkdev_file_llseek(struct fs_file *file, loff_t offset,
				 int whence)
{
	struct block_dev *bd = file->private_data;
	loff_t new_pos;

	if (!bd || bd->dead)
		return -ENODEV;

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = file->pos + offset;
		break;
	case SEEK_END:
		new_pos = (loff_t)blkdev_capacity_bytes(bd) + offset;
		break;
	default:
		return -EINVAL;
	}

	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int blkdev_file_iterate_shared(struct fs_file *file,
				      struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int blkdev_file_fsync(struct fs_file *file, loff_t start, loff_t end,
			     int datasync)
{
	struct block_dev *bd = file->private_data;
	int err;

	(void)datasync;
	if (!bd || bd->dead)
		return -ENODEV;

	err = page_cache_flush_range(bd->bd_mapping, start, end);
	if (!err)
		err = blkdev_flush(bd);
	return err;
}

static int blkdev_file_flush(struct fs_file *file)
{
	(void)file;
	return 0;
}

static long blkdev_file_ioctl(struct fs_file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct block_dev *bd = file->private_data;
	u64 bytes;

	if (!bd || bd->dead)
		return -ENODEV;

	switch (cmd) {
	case BLKGETSIZE64:
		if (!arg)
			return -EFAULT;
		bytes = blkdev_capacity_bytes(bd);
		return copy_to_user((void *)arg, &bytes, sizeof(bytes)) ?
			       -EFAULT :
			       0;
	case BLKFLSBUF: {
		int err = page_cache_flush(bd->bd_mapping);

		if (!err)
			err = blkdev_flush(bd);
		return err;
	}
	default:
		return -ENOTTY;
	}
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
