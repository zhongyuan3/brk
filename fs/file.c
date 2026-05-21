#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/lock.h>
#include <brk/path.h>
#include <brk/slab.h>
#include <brk/types.h>

static struct kobj_pool file_cache;

void file_cache_init(void)
{
	kobj_pool_init(&file_cache, sizeof(struct opened_file),
		       alignof(struct opened_file), "file_cache");
}

struct opened_file *file_alloc(struct file_anchor *path, fmode_t mode)
{
	struct opened_file *file;

	file = kobj_pool_alloc(&file_cache);
	if (!file)
		return ERR_PTR(-ENOMEM);

	refcnt_init(&file->f_count, 1);
	path_dup(path);
	file->f_path = *path;
	file->f_inode = path->dentry->d_inode;
	file->f_op = file->f_inode->i_fop;

	spinlock_init(&file->f_lock, "file.f_lock");
	file->f_mode = mode;
	sleeplock_init(&file->f_pos_lock, "file.f_pos_lock");
	file->f_pos = 0;

	int err = file->f_op->open(file->f_inode, file);
	if (err) {
		path_put(&file->f_path);
		kobj_pool_free(&file_cache, file);
		return ERR_PTR(err);
	}

	return file;
}

struct opened_file *file_dup(struct opened_file *file)
{
	refcnt_inc(&file->f_count);
	return file;
}

void file_put(struct opened_file *file)
{
	const struct opened_file_ops *fop;

	if (refcnt_dec_fetch(&file->f_count) > 0)
		return;

	fop = file->f_op;
	if (fop->release)
		fop->release(file->f_inode, file);

	path_put(&file->f_path);

	kobj_pool_free(&file_cache, file);
}

loff_t file_lseek(struct opened_file *file, loff_t len, int whence)
{
	loff_t ret = file->f_op->llseek(file, len, whence);
	if (ret >= 0)
		file->f_pos = ret;
	return ret;
}

ssize_t file_read(struct opened_file *file, void *buf, usize_t size)
{
	loff_t *pos = &file->f_pos;
	sleeplock_acquire(&file->f_pos_lock);
	ssize_t ret = file->f_op->read(file, buf, size, pos);
	sleeplock_release(&file->f_pos_lock);
	return ret;
}

ssize_t file_write(struct opened_file *file, const void *buf, usize_t size)
{
	loff_t *pos = &file->f_pos;
	sleeplock_acquire(&file->f_pos_lock);
	ssize_t ret = file->f_op->write(file, buf, size, pos);
	sleeplock_release(&file->f_pos_lock);
	return ret;
}

long file_ioctl(struct opened_file *file, unsigned int cmd, unsigned long arg)
{
	if (!file->f_op->ioctl)
		return -ENOTTY;
	return file->f_op->ioctl(file, cmd, arg);
}

int file_stat(struct opened_file *file, struct stat *buf)
{
	const struct fs_inode_ops *i_op = file->f_inode->i_op;
	return i_op->getattr(&file->f_path, buf, 0, 0);
}

int file_truncate(struct opened_file *file, loff_t size)
{
	(void)file;
	(void)size;
	return 0;
}
