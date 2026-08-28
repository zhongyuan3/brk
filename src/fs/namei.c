#include <brk/base/error.h>
#include <brk/fs/fs.h>
#include <brk/fs/path.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/fcntl.h>

struct fs_file *do_openat(int dirfd, const char *pathname, int flags,
			  umode_t mode)
{
	int err;
	struct fs_file *file;
	struct fs_path path;

	if ((flags & O_RDWR) && (flags & O_WRONLY))
		return ERR_PTR(-EINVAL);

	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return ERR_PTR(err);

	spinlock_acquire(&path.dentry->lock);
	bool exists = !(path.dentry->flags & DCACHE_NEGATIVE);
	spinlock_release(&path.dentry->lock);

	bool create = (flags & O_CREAT) != 0;
	if (!exists && !create) {
		fs_path_put(&path);
		return ERR_PTR(-ENOENT);
	}

	bool excl = (flags & O_EXCL) != 0;
	if (exists && excl) {
		fs_path_put(&path);
		return ERR_PTR(-EEXIST);
	}

	if (!exists && create) {
		struct fs_path parent_path;
		err = fs_path_dot_dot(&path, &parent_path);
		if (err) {
			fs_path_put(&path);
			return ERR_PTR(err);
		}
		struct fs_inode *inode = parent_path.dentry->inode;
		sleeplock_acquire(&inode->rwsem);
		err = inode->ops->create(inode, path.dentry, mode, excl);
		sleeplock_release(&inode->rwsem);
		if (err) {
			fs_path_put(&parent_path);
			fs_path_put(&path);
			return ERR_PTR(err);
		}
		fs_path_put(&parent_path);
	}

	fmode_t fmode = 0;

	if (flags == O_RDONLY)
		fmode = FMODE_READ;

	if (flags & O_WRONLY)
		fmode = FMODE_WRITE;

	if (flags & O_RDWR)
		fmode = FMODE_READ | FMODE_WRITE;

	if (S_ISDIR(path.dentry->inode->mode))
		fmode |= FMODE_DIR;

	file = fs_file_alloc(&path, fmode);
	if (IS_ERR(file)) {
		fs_path_put(&path);
		return ERR_CAST(file);
	}
	fs_path_put(&path);

	if (flags & O_TRUNC) {
		err = fs_file_truncate(file, 0);
		if (err) {
			fs_file_put(file);
			return ERR_PTR(err);
		}
	}

	if (flags & O_APPEND) {
		off_t ret = fs_file_lseek(file, 0, SEEK_END);
		if (ret < 0) {
			fs_file_put(file);
			return ERR_PTR(ret);
		}
	}

	return file;
}

int do_mkdirat(int dirfd, const char *pathname, umode_t mode)
{
	int err;
	struct fs_path path;

	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->mkdir(inode, path.dentry, mode | S_IFDIR);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}

int do_mknodat(int dirfd, const char *pathname, umode_t mode, dev_t dev)
{
	int err;
	struct fs_path path;

	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->mknod(inode, path.dentry, mode, dev);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}

int do_linkat(int olddirfd, const char *oldpathname, int newdirfd,
	      const char *newpathname, int flags)
{
	(void)flags;

	int err;
	struct fs_path oldpath, newpath;

	err = fs_path_lookup_at(olddirfd, oldpathname, 0, &oldpath);
	if (err)
		return err;

	err = fs_path_lookup_at(newdirfd, newpathname, 0, &newpath);
	if (err) {
		fs_path_put(&oldpath);
		return err;
	}

	spinlock_acquire(&newpath.dentry->lock);
	if (!(newpath.dentry->flags & DCACHE_NEGATIVE)) {
		spinlock_release(&newpath.dentry->lock);
		fs_path_put(&oldpath);
		fs_path_put(&newpath);
		return -EEXIST;
	}
	spinlock_release(&newpath.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&newpath, &parent_path);
	if (err) {
		fs_path_put(&oldpath);
		fs_path_put(&newpath);
		return err;
	}
	struct fs_inode *parent_inode = parent_path.dentry->inode;
	struct fs_inode *old_inode = oldpath.dentry->inode;
	sleeplock_acquire(&parent_inode->rwsem);
	sleeplock_acquire(&old_inode->rwsem);
	err = parent_inode->ops->link(oldpath.dentry, parent_inode,
				      newpath.dentry);
	sleeplock_release(&old_inode->rwsem);
	sleeplock_release(&parent_inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&oldpath);
	fs_path_put(&newpath);
	return err;
}

int do_unlinkat(int dirfd, const char *pathname, int flags)
{
	int err;
	struct fs_path path;

	(void)flags;

	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->unlink(inode, path.dentry);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}

int do_symlinkat(int dirfd, const char *pathname, const char *target)
{
	int err;
	struct fs_path path;

	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->symlink(inode, path.dentry, target);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}

int do_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz)
{
	int err;
	struct fs_path path;

	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->lock);

	struct fs_inode *inode = path.dentry->inode;
	if (!S_ISLNK(inode->mode)) {
		fs_path_put(&path);
		return -EINVAL;
	}

	if (!inode->ops->readlink) {
		fs_path_put(&path);
		return -EOPNOTSUPP;
	}

	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->readlink(path.dentry, buf, bufsiz);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&path);

	return err;
}

int do_creat(const char *pathname, umode_t mode)
{
	int err;
	struct fs_path path;

	err = fs_path_lookup(pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->create(inode, path.dentry, mode, true);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}

int do_renameat(int olddirfd, const char *oldpathname, int newdirfd,
		const char *newpathname, unsigned int flags)
{
	int err;
	struct fs_path oldpath, newpath;

	err = fs_path_lookup_at(olddirfd, oldpathname, 0, &oldpath);
	if (err)
		return err;

	err = fs_path_lookup_at(newdirfd, newpathname, 0, &newpath);
	if (err) {
		fs_path_put(&oldpath);
		return err;
	}

	spinlock_acquire(&newpath.dentry->lock);
	if (!(newpath.dentry->flags & DCACHE_NEGATIVE)) {
		spinlock_release(&newpath.dentry->lock);
		fs_path_put(&oldpath);
		fs_path_put(&newpath);
		return -EEXIST;
	}
	spinlock_release(&newpath.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&newpath, &parent_path);
	if (err) {
		fs_path_put(&oldpath);
		fs_path_put(&newpath);
		return err;
	}
	struct fs_inode *parent_inode = parent_path.dentry->inode;
	struct fs_inode *old_inode = oldpath.dentry->inode;
	sleeplock_acquire(&parent_inode->rwsem);
	sleeplock_acquire(&old_inode->rwsem);
	err = parent_inode->ops->rename(old_inode, oldpath.dentry, parent_inode,
					newpath.dentry, flags);
	sleeplock_release(&old_inode->rwsem);
	sleeplock_release(&parent_inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&oldpath);
	fs_path_put(&newpath);
	return err;
}

int do_rmdir(const char *pathname)
{
	int err;
	struct fs_path path;

	err = fs_path_lookup_at(AT_FDCWD, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->rmdir(inode, path.dentry);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}
