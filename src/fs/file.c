#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/initcall.h>
#include <brk/kernel.h>
#include <brk/path.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <uapi/errno.h>

static struct slab_allocator file_cache;

void fs_file_cache_init(void)
{
	slab_init(&file_cache, sizeof(struct fs_file), alignof(struct fs_file),
		  "file_cache");
}
core_initcall(fs_file_cache_init);

struct fs_file *fs_file_alloc(struct fs_path *path, fmode_t mode)
{
	struct fs_file *file;
	struct fs_inode *inode;

	inode = fs_dentry_inode(path->dentry);
	if (!inode)
		return ERR_PTR(-ENOENT);

	file = slab_alloc(&file_cache);
	if (!file)
		return ERR_PTR(-ENOMEM);

	refcnt_init(&file->count, 1);
	fs_path_get(path);
	file->path = *path;
	file->inode = inode;
	file->ops = inode->fops;

	spinlock_init(&file->lock, "file.f_lock");
	file->mode = mode;
	sleeplock_init(&file->pos_lock, "file.f_pos_lock");
	file->pos = 0;
	file->private_data = NULL;
	file->dev_private = NULL;

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
	loff_t ret;

	/* Serialize with fs_file_read()/write() and getdents(). */
	sleeplock_acquire(&file->pos_lock);
	ret = file->ops->llseek(file, len, whence);
	if (ret >= 0)
		file->pos = ret;
	sleeplock_release(&file->pos_lock);
	return ret;
}

ssize_t fs_file_read(struct fs_file *file, void *buf, size_t size)
{
	loff_t *pos = &file->pos;
	sleeplock_acquire(&file->pos_lock);
	ssize_t ret = file->ops->read(file, buf, size, pos);
	sleeplock_release(&file->pos_lock);
	return ret;
}

ssize_t fs_file_write(struct fs_file *file, const void *buf, size_t size)
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
	struct fs_iattr attr = {
		.valid = ATTR_SIZE,
		.size = size,
		.file = file,
	};

	if (!file || !file->inode)
		return -EINVAL;
	if (size < 0)
		return -EINVAL;

	/* O_TRUNC on devices/FIFOs/directories is a no-op. */
	if (!S_ISREG(file->inode->mode))
		return 0;
	if (!file->inode->ops || !file->inode->ops->setattr)
		return -EOPNOTSUPP;

	return file->inode->ops->setattr(file->path.dentry, &attr);
}
