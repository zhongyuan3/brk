#include <brk/fs/dcache.h>
#include <brk/fs/fs.h>
#include <brk/fs/path.h>
#include <brk/kernel/refcnt.h>
#include <brk/lib/error.h>
#include <brk/lib/kernel.h>
#include <brk/lib/types.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/slab.h>
#include <uapi/brk/errno.h>

static struct slab_allocator file_cache;

void fs_file_cache_init(void)
{
	slab_init(&file_cache, sizeof(struct fs_file), alignof(struct fs_file),
		  "file_cache");
}

struct fs_file *fs_file_alloc(struct fs_path *path, fmode_t mode)
{
	struct fs_file *file;

	file = slab_alloc(&file_cache);
	if (!file)
		return ERR_PTR(-ENOMEM);

	refcnt_init(&file->count, 1);
	fs_path_get(path);
	file->path = *path;
	file->inode = path->dentry->inode;
	file->ops = file->inode->fops;

	spinlock_init(&file->lock, "file.f_lock");
	file->mode = mode;
	sleeplock_init(&file->pos_lock, "file.f_pos_lock");
	file->pos = 0;

	int err = file->ops->open(file->inode, file);
	if (err) {
		fs_path_put(&file->path);
		slab_free(&file_cache, file);
		return ERR_PTR(err);
	}

	return file;
}

struct fs_file *fs_file_get(struct fs_file *file)
{
	refcnt_inc(&file->count);
	return file;
}

void fs_file_put(struct fs_file *file)
{
	const struct fs_file_ops *fop;

	if (refcnt_dec_fetch(&file->count) > 0)
		return;

	fop = file->ops;
	if (fop->release)
		fop->release(file->inode, file);

	fs_path_put(&file->path);

	slab_free(&file_cache, file);
}

loff_t fs_file_lseek(struct fs_file *file, loff_t len, int whence)
{
	loff_t ret = file->ops->llseek(file, len, whence);
	if (ret >= 0)
		file->pos = ret;
	return ret;
}

ssize_t fs_file_read(struct fs_file *file, void *buf, usize_t size)
{
	loff_t *pos = &file->pos;
	sleeplock_acquire(&file->pos_lock);
	ssize_t ret = file->ops->read(file, buf, size, pos);
	sleeplock_release(&file->pos_lock);
	return ret;
}

ssize_t fs_file_write(struct fs_file *file, const void *buf, usize_t size)
{
	loff_t *pos = &file->pos;
	sleeplock_acquire(&file->pos_lock);
	ssize_t ret = file->ops->write(file, buf, size, pos);
	sleeplock_release(&file->pos_lock);
	return ret;
}

long fs_file_ioctl(struct fs_file *file, unsigned int cmd, unsigned long arg)
{
	if (!file->ops->ioctl)
		return -ENOTTY;
	return file->ops->ioctl(file, cmd, arg);
}

int fs_file_stat(struct fs_file *file, struct stat *buf)
{
	const struct fs_inode_ops *i_op = file->inode->ops;
	return i_op->getattr(&file->path, buf, 0, 0);
}

int fs_file_truncate(struct fs_file *file, loff_t size)
{
	(void)file;
	(void)size;
	return 0;
}
