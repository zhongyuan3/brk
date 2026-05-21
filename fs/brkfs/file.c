#include "brkfs.h"
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/string.h>

/*
 * Page cache backing for brkfs regular files.
 *
 * ->readpage and ->writepage talk to the block device a logical disk-block
 * at a time, transparently handling holes (read as zeroes; write allocates
 * a fresh data block).
 */

static int brkfs_readpage(struct page_cache *m, struct cached_page *cp)
{
	struct fs_inode *inode = m->host;
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t off = (loff_t)cp->index << PAGE_SHIFT;
	u8 *data = cached_page_addr(cp);

	if (bs > PAGE_SIZE || PAGE_SIZE % bs != 0) {
		klog_warn("%s(): unsupported blocksize %u\n", __func__, bs);
		return -EIO;
	}

	for (usize_t boff = 0; boff < PAGE_SIZE; boff += bs) {
		u32 bno;
		int err;

		err = brkfs_inode_getblk(inode, off + (loff_t)boff, &bno, 0,
					 sbi);
		if (err)
			return err;
		if (bno == 0) {
			/* hole (or past EOF): zero-fill */
			memset(data + boff, 0, bs);
			continue;
		}
		err = brkfs_block_read(sbi, bno, data + boff);
		if (err)
			return err;
	}
	return 0;
}

static int brkfs_writepage(struct page_cache *m, struct cached_page *cp)
{
	struct fs_inode *inode = m->host;
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t off = (loff_t)cp->index << PAGE_SHIFT;
	u8 *data = cached_page_addr(cp);
	int ret = 0;

	if (bs > PAGE_SIZE || PAGE_SIZE % bs != 0) {
		klog_warn("%s(): unsupported blocksize %u\n", __func__, bs);
		return -EIO;
	}

	for (usize_t boff = 0; boff < PAGE_SIZE; boff += bs) {
		u32 bno;
		int err;

		err = brkfs_inode_getblk(inode, off + (loff_t)boff, &bno,
					 BRKFS_GETBLK_CREATE, sbi);
		if (err)
			return err;
		if (bno == 0)
			return -ENOSPC;
		err = brkfs_block_write(sbi, bno, data + boff);
		if (err) {
			ret = err;
			break;
		}
	}

	/* Persist the (possibly newly-allocated) block pointers in the inode
	 * so writepage is durable across crashes / unmounts. */
	if (!ret)
		ret = brkfs_inode_write(sbi, inode);
	return ret;
}

const struct page_cache_ops brkfs_aops = {
	.readpage = brkfs_readpage,
	.writepage = brkfs_writepage,
};

static int brkfs_file_open(struct fs_inode *inode, struct opened_file *file)
{
	int ret = 0;
	umode_t imode = inode->i_mode;

	sleeplock_acquire(&inode->i_rwsem);
	if (S_ISCHR(imode))
		file->f_op = &chrdev_fops;
	else if (S_ISBLK(imode))
		file->f_op = &blkdev_fops;
	else if (S_ISREG(imode))
		file->f_op = &brkfs_file_fops;
	else if (S_ISDIR(imode))
		file->f_op = &brkfs_dir_fops;
	else if (S_ISLNK(imode))
		file->f_op = &brkfs_file_fops;
	else
		ret = -EINVAL;
	sleeplock_release(&inode->i_rwsem);
	return ret;
}

static int brkfs_file_release(struct fs_inode *inode, struct opened_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t brkfs_file_read(struct opened_file *file, char *buf,
			       usize_t size, loff_t *pos)
{
	struct fs_inode *inode = file->f_inode;

	if (inode->i_mapping)
		return generic_file_read(file, buf, size, pos);

	/* Fallback path (no page cache attached, e.g. for symlinks read via
	 * brkfs_readlink that does not go through the fs_file_ops). */
	usize_t rd = 0;
	int err;

	sleeplock_acquire(&inode->i_rwsem);
	err = brkfs_file_read_at(inode, pos, buf, size, &rd);
	sleeplock_release(&inode->i_rwsem);
	if (err)
		return err;
	return (ssize_t)rd;
}

static ssize_t brkfs_file_write(struct opened_file *file, const char *buf,
				usize_t size, loff_t *pos)
{
	struct fs_inode *inode = file->f_inode;

	if (inode->i_mapping)
		return generic_file_write(file, buf, size, pos);

	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	usize_t wr = 0;
	int err;

	sleeplock_acquire(&inode->i_rwsem);
	err = brkfs_file_write_at(inode, pos, buf, size, &wr);
	if (!err)
		err = brkfs_inode_write(sbi, inode);
	sleeplock_release(&inode->i_rwsem);
	if (err)
		return err;
	return (ssize_t)wr;
}

static loff_t brkfs_file_llseek(struct opened_file *file, loff_t offset,
				int whence)
{
	struct fs_inode *inode = file->f_inode;
	loff_t new_pos = 0;

	if (whence == SEEK_SET)
		new_pos = offset;
	else if (whence == SEEK_CUR)
		new_pos = file->f_pos + offset;
	else if (whence == SEEK_END)
		new_pos = inode->i_size + offset;
	else
		return -EINVAL;
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int brkfs_file_iterate_shared(struct opened_file *file,
				     struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int brkfs_file_fsync(struct opened_file *file, loff_t start, loff_t end,
			    int datasync)
{
	struct fs_inode *inode = file->f_inode;
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	int err;

	(void)start;
	(void)end;
	(void)datasync;

	sleeplock_acquire(&inode->i_rwsem);
	err = filemap_writeback(inode->i_mapping);
	if (!err)
		err = brkfs_inode_write(sbi, inode);
	sleeplock_release(&inode->i_rwsem);
	return err;
}

static int brkfs_file_flush(struct opened_file *file)
{
	(void)file;
	return 0;
}

static long brkfs_file_ioctl(struct opened_file *file, unsigned int cmd,
			     unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

const struct opened_file_ops brkfs_file_fops = {
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
