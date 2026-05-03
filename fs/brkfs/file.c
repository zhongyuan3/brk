#include "brkfs.h"
#include <brk/errno.h>
#include <brk/fs.h>

static int brkfs_file_open(struct inode *inode, struct file *file)
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

static int brkfs_file_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t brkfs_file_read(struct file *file, char *buf, size_t size,
			       loff_t *pos)
{
	struct inode *inode = file->f_inode;
	size_t rd = 0;
	int err;

	sleeplock_acquire(&inode->i_rwsem);
	err = brkfs_file_read_at(inode, pos, buf, size, &rd);
	sleeplock_release(&inode->i_rwsem);
	if (err)
		return err;
	return (ssize_t)rd;
}

static ssize_t brkfs_file_write(struct file *file, const char *buf, size_t size,
				loff_t *pos)
{
	struct inode *inode = file->f_inode;
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	size_t wr = 0;
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

static loff_t brkfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_inode;
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

static int brkfs_file_iterate_shared(struct file *file, struct dir_context *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int brkfs_file_fsync(struct file *file, loff_t start, loff_t end,
			    int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int brkfs_file_flush(struct file *file)
{
	(void)file;
	return 0;
}

static long brkfs_file_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

const struct file_operations brkfs_file_fops = {
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
