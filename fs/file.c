#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/mutex.h>
#include <brk/path.h>
#include <brk/permission.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <stdalign.h>
#include <uapi/errno.h>
#include <uapi/poll.h>
#include <uapi/stat.h>

static struct slab_allocator file_cache;

void file_cache_init(void)
{
	slab_init(&file_cache, sizeof(struct file), alignof(struct file),
		  "file_cache");
}

struct file *file_alloc(struct path *path, fmode_t mode)
{
	struct file *file;
	struct inode *inode;

	inode = dentry_inode(path->dentry);
	if (!inode)
		return ERR_PTR(-ENOENT);

	file = slab_alloc(&file_cache);
	if (!file)
		return ERR_PTR(-ENOMEM);

	refcnt_init(&file->count, 1);
	path_get(path);
	file->path = *path;
	inode_get(inode);
	file->inode = inode;
	file->ops = inode->fops;

	spinlock_init(&file->lock, "file.f_lock");
	file->mode = mode;
	file->anon = false;
	mutex_init(&file->pos_lock, "file.f_pos_lock");
	file->pos = 0;
	file->private_data = NULL;
	file->dev_private = NULL;

	int err = file->ops->open(file->inode, file);
	if (err) {
		path_put(&file->path);
		inode_put(file->inode);
		slab_free(&file_cache, file);
		return ERR_PTR(err);
	}

	return file;
}

struct file *file_get(struct file *file)
{
	refcnt_inc(&file->count);
	return file;
}

struct file *file_alloc_anon(const struct file_ops *ops, void *private_data)
{
	struct file *file;

	if (!ops)
		return ERR_PTR(-EINVAL);

	file = slab_alloc(&file_cache);
	if (!file)
		return ERR_PTR(-ENOMEM);

	memset(&file->path, 0, sizeof(file->path));
	refcnt_init(&file->count, 1);
	file->inode = NULL;
	file->ops = ops;
	spinlock_init(&file->lock, "file.f_lock");
	file->mode = FMODE_READ | FMODE_WRITE;
	file->anon = true;
	mutex_init(&file->pos_lock, "file.f_pos_lock");
	file->pos = 0;
	file->private_data = private_data;
	file->dev_private = NULL;
	return file;
}

void file_put(struct file *file)
{
	const struct file_ops *fop;

	if (refcnt_dec_fetch(&file->count) > 0)
		return;

	fop = file->ops;
	if (fop->release)
		fop->release(file->inode, file);

	if (!file->anon) {
		path_put(&file->path);
		inode_put(file->inode);
	}

	slab_free(&file_cache, file);
}

loff_t file_lseek(struct file *file, loff_t len, int whence)
{
	loff_t ret;

	/* Serialize with file_read()/write() and getdents(). */
	mutex_lock(&file->pos_lock);
	ret = file->ops->llseek(file, len, whence);
	if (ret >= 0)
		file->pos = ret;
	mutex_unlock(&file->pos_lock);
	return ret;
}

ssize_t file_read(struct file *file, void *buf, size_t size)
{
	loff_t *pos = &file->pos;
	mutex_lock(&file->pos_lock);
	ssize_t ret = file->ops->read(file, buf, size, pos);
	mutex_unlock(&file->pos_lock);
	return ret;
}

void remove_inode_privs(struct inode *inode, struct dentry *dentry)
{
	struct iattr attr;

	if (!inode || !dentry || !inode->ops || !inode->ops->setattr)
		return;
	if (capable(CAP_FSETID))
		return;
	if (!(inode->mode & (S_ISUID | S_ISGID)))
		return;

	attr.valid = ATTR_MODE;
	attr.mode = inode->mode & ~(S_ISUID | S_ISGID);
	attr.file = NULL;
	inode->ops->setattr(dentry, &attr);
}

void file_remove_privs(struct file *file)
{
	if (!file || file->anon || !file->inode)
		return;
	remove_inode_privs(file->inode, file->path.dentry);
}

ssize_t file_write(struct file *file, const void *buf, size_t size)
{
	loff_t *pos = &file->pos;

	file_remove_privs(file);

	mutex_lock(&file->pos_lock);
	ssize_t ret = file->ops->write(file, buf, size, pos);
	mutex_unlock(&file->pos_lock);
	return ret;
}

long file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (!file->ops->ioctl)
		return -ENOTTY;
	return file->ops->ioctl(file, cmd, arg);
}

unsigned int file_poll(struct file *file, unsigned int events)
{
	if (!file)
		return POLLNVAL;
	if (file->ops && file->ops->poll)
		return file->ops->poll(file, events);

	/* Regular files are always ready for both directions. */
	return (POLLIN | POLLOUT) & (events | POLLERR | POLLHUP);
}

int file_stat(struct file *file, struct kstat *buf)
{
	const struct inode_ops *i_op;

	if (file->anon) {
		memset(buf, 0, sizeof(*buf));
		buf->mode = S_IFSOCK | 0777;
		buf->nlink = 1;
		return 0;
	}

	i_op = file->inode->ops;
	return i_op->getattr(&file->path, buf, 0, 0);
}

int file_truncate(struct file *file, loff_t size)
{
	struct iattr attr = {
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

	file_remove_privs(file);

	return file->inode->ops->setattr(file->path.dentry, &attr);
}

int file_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	if (!file || !file->inode)
		return -EINVAL;
	if (!file->ops || !file->ops->fallocate)
		return -EOPNOTSUPP;

	file_remove_privs(file);

	return file->ops->fallocate(file, mode, offset, len);
}
