#include "brkfs.h"
#include <arch/page.h>
#include <brk/blkdev.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/sleeplock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

static int brkfs_file_read_page(struct page_cache *m, struct cached_page *cp)
{
	struct fs_inode *inode = m->host;
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t off = (loff_t)cp->index << PAGE_SHIFT;
	u8 *data = cached_page_addr(cp);
	struct block_dev *bd = sbi->s_bdev;

	if (bs > PAGE_SIZE || PAGE_SIZE % bs != 0) {
		klog_warn("%s(): unsupported blocksize %u\n", __func__, bs);
		return -EIO;
	}

	for (usize_t boff = 0; boff < PAGE_SIZE; boff += bs) {
		u32 bno;
		int err;

		err = brkfs_inode_getblk(inode, off + boff, &bno, 0, sbi);
		if (err)
			return err;

		if (bno == 0) {
			/* hole (or past EOF): zero-fill */
			memset(data + boff, 0, bs);
			continue;
		}

		u32 sec_cnt = bs / bd->phy_bsize;
		u64 sector = bno * sec_cnt;
		err = blkdev_read(bd, sector, data + boff, sec_cnt);
		if (err)
			return err;
	}
	return 0;
}

static int brkfs_file_write_page(struct page_cache *m, struct cached_page *cp)
{
	struct fs_inode *inode = m->host;
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t off = (loff_t)cp->index << PAGE_SHIFT;
	u8 *data = cached_page_addr(cp);
	int ret = 0;
	struct block_dev *bd = sbi->s_bdev;

	if (bs > PAGE_SIZE || PAGE_SIZE % bs != 0) {
		klog_warn("%s(): unsupported blocksize %u\n", __func__, bs);
		return -EIO;
	}

	for (usize_t boff = 0; boff < PAGE_SIZE; boff += bs) {
		u32 bno;
		int err;

		err = brkfs_inode_getblk(inode, off + boff, &bno,
					 BRKFS_GETBLK_CREATE, sbi);
		if (err)
			return err;
		if (bno == 0)
			return -ENOSPC;

		u32 sec_cnt = bs / bd->phy_bsize;
		u64 sector = bno * sec_cnt;
		err = blkdev_write(bd, sector, data + boff, sec_cnt);
		if (err) {
			ret = err;
			break;
		}
	}

	if (ret == 0)
		ret = brkfs_inode_write(sbi, inode);
	return ret;
}

const struct page_cache_ops brkfs_file_pc_ops = {
	.read_page = brkfs_file_read_page,
	.write_page = brkfs_file_write_page,
};

static int brkfs_file_open(struct fs_inode *inode, struct fs_file *file)
{
	int ret = 0;
	umode_t imode = inode->mode;

	if (S_ISCHR(imode))
		file->ops = &chrdev_fops;
	else if (S_ISBLK(imode))
		file->ops = &blkdev_fops;
	else if (S_ISREG(imode))
		file->ops = &brkfs_file_fops;
	else if (S_ISDIR(imode))
		file->ops = &brkfs_dir_fops;
	else if (S_ISLNK(imode))
		file->ops = &brkfs_file_fops;
	else
		ret = -EINVAL;

	return ret;
}

static int brkfs_file_release(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t brkfs_file_read(struct fs_file *file, char *buf, usize_t size,
			       loff_t *pos)
{
	return generic_file_read(file, buf, size, pos);
}

static ssize_t brkfs_file_write(struct fs_file *file, const char *buf,
				usize_t size, loff_t *pos)
{
	return generic_file_write(file, buf, size, pos);
}

static loff_t brkfs_file_llseek(struct fs_file *file, loff_t offset, int whence)
{
	struct fs_inode *inode = file->inode;
	loff_t new_pos = 0;

	if (whence == SEEK_SET)
		new_pos = offset;
	else if (whence == SEEK_CUR)
		new_pos = file->pos + offset;
	else if (whence == SEEK_END)
		new_pos = inode->size + offset;
	else
		return -EINVAL;
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int brkfs_file_iterate_shared(struct fs_file *file,
				     struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int brkfs_file_fsync(struct fs_file *file, loff_t start, loff_t end,
			    int datasync)
{
	struct fs_inode *inode = file->inode;
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	int err;

	(void)datasync;

	sleeplock_acquire(&inode->rwsem);
	err = page_cache_flush_range(inode->mapping, start, end);
	if (!err)
		err = brkfs_inode_write(sbi, inode);
	sleeplock_release(&inode->rwsem);
	return err;
}

static int brkfs_file_flush(struct fs_file *file)
{
	(void)file;
	return 0;
}

static long brkfs_file_ioctl(struct fs_file *file, unsigned int cmd,
			     unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

int brkfs_file_read_at(struct fs_inode *inode, loff_t *pos, void *buf,
		       usize_t size, usize_t *read_out)
{
	struct brkfs_inode_info *inf = inode->private_data;
	loff_t off = *pos;
	int err = 0;
	struct cached_page *cp;

	if (!inf || (!S_ISREG(inode->mode) && !S_ISLNK(inode->mode))) {
		*read_out = 0;
		return -EINVAL;
	}
	if (off >= inode->size) {
		*read_out = 0;
		return 0;
	}
	if ((loff_t)(off + size) > inode->size)
		size = (usize_t)(inode->size - off);

	u8 *p = buf;
	while (size > 0) {
		usize_t in_off = off % PAGE_SIZE;
		usize_t chunk = PAGE_SIZE - in_off;
		if (chunk > size)
			chunk = size;

		cp = read_mapping_page(inode->mapping, off >> PAGE_SHIFT);
		if (IS_ERR(cp)) {
			err = PTR_ERR(cp);
			break;
		}

		cached_page_lock(cp);
		u8 *data = cached_page_addr(cp);
		memcpy(p, data + in_off, chunk);
		cached_page_unlock(cp);
		cached_page_put(cp);

		off += chunk;
		size -= chunk;
		p += chunk;
	}

	*read_out = p - (u8 *)buf;
	*pos = off;
	return err;
}

int brkfs_file_write_at(struct fs_inode *inode, loff_t *pos, const void *buf,
			usize_t size, usize_t *written_out)
{
	struct brkfs_inode_info *inf = inode->private_data;
	loff_t off = *pos;
	int err = 0;
	struct cached_page *cp;

	if (!inf || (!S_ISREG(inode->mode) && !S_ISLNK(inode->mode))) {
		*written_out = 0;
		return -EINVAL;
	}

	const u8 *p = buf;
	while (size > 0) {
		usize_t in_off = off % PAGE_SIZE;
		usize_t chunk = PAGE_SIZE - in_off;
		if (chunk > size)
			chunk = size;

		cp = read_mapping_page(inode->mapping, off >> PAGE_SHIFT);
		if (IS_ERR(cp)) {
			err = PTR_ERR(cp);
			break;
		}

		cached_page_lock(cp);
		u8 *data = cached_page_addr(cp);
		memcpy(data + in_off, p, chunk);
		cached_page_mark_dirty(cp);
		cached_page_unlock(cp);
		cached_page_put(cp);

		off += chunk;
		p += chunk;
		size -= chunk;
	}

	*pos = off;
	if (written_out)
		*written_out = p - (const u8 *)buf;
	if (off > inode->size) {
		inode->size = off;
		fs_inode_mark_dirty(inode);
	}
	inode_touch_mtime(inode);

	return err;
}

const struct fs_file_ops brkfs_file_fops = {
	.open = brkfs_file_open,
	.release = brkfs_file_release,
	.read = brkfs_file_read,
	.write = brkfs_file_write,
	.llseek = brkfs_file_llseek,
	.iterate_shared = brkfs_file_iterate_shared,
	.fsync = brkfs_file_fsync,
	.flush = brkfs_file_flush,
	.ioctl = brkfs_file_ioctl,
};
