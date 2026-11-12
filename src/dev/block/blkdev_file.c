#include <brk/blkdev.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/pagecache.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/ioctl.h>

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
