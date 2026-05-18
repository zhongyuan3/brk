#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fcntl.h>
#include <brk/fs.h>
#include <brk/lock.h>
#include <brk/path.h>

struct file *do_openat(int dirfd, const char *pathname, int flags, umode_t mode)
{
	int err;
	struct file *file;
	struct path path;

	if ((flags & O_RDWR) && (flags & O_WRONLY))
		return ERR_PTR(-EINVAL);

	err = path_lookupat(dirfd, pathname, 0, &path);
	if (err)
		return ERR_PTR(err);

	spinlock_acquire(&path.dentry->d_lock);
	bool exists = !(path.dentry->d_flags & DCACHE_NEGATIVE);
	spinlock_release(&path.dentry->d_lock);

	bool create = (flags & O_CREAT) != 0;
	if (!exists && !create) {
		path_put(&path);
		return ERR_PTR(-ENOENT);
	}

	bool excl = (flags & O_EXCL) != 0;
	if (exists && excl) {
		path_put(&path);
		return ERR_PTR(-EEXIST);
	}

	if (!exists && create) {
		struct path parent_path;
		err = path_dot_dot(&path, &parent_path);
		if (err) {
			path_put(&path);
			return ERR_PTR(err);
		}
		struct inode *inode = parent_path.dentry->d_inode;
		sleeplock_acquire(&inode->i_rwsem);
		err = inode->i_op->create(inode, path.dentry, mode, excl);
		sleeplock_release(&inode->i_rwsem);
		if (err) {
			path_put(&parent_path);
			path_put(&path);
			return ERR_PTR(err);
		}
		path_put(&parent_path);
	}

	fmode_t fmode = 0;

	if (flags == O_RDONLY)
		fmode = FMODE_READ;

	if (flags & O_WRONLY)
		fmode = FMODE_WRITE;

	if (flags & O_RDWR)
		fmode = FMODE_READ | FMODE_WRITE;

	if (S_ISDIR(path.dentry->d_inode->i_mode))
		fmode |= FMODE_DIR;

	file = file_alloc(&path, fmode);
	if (IS_ERR(file)) {
		path_put(&path);
		return ERR_CAST(file);
	}
	path_put(&path);

	if (flags & O_TRUNC) {
		err = file_truncate(file, 0);
		if (err) {
			file_put(file);
			return ERR_PTR(err);
		}
	}

	if (flags & O_APPEND) {
		off_t ret = file_lseek(file, 0, SEEK_END);
		if (ret < 0) {
			file_put(file);
			return ERR_PTR(ret);
		}
	}

	return file;
}

int do_mkdirat(int dirfd, const char *pathname, umode_t mode)
{
	int err;
	struct path path;

	err = path_lookupat(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (!(path.dentry->d_flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->d_inode;
	sleeplock_acquire(&inode->i_rwsem);
	err = inode->i_op->mkdir(inode, path.dentry, mode | S_IFDIR);
	sleeplock_release(&inode->i_rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_mknodat(int dirfd, const char *pathname, umode_t mode, dev_t dev)
{
	int err;
	struct path path;

	err = path_lookupat(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (!(path.dentry->d_flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->d_inode;
	sleeplock_acquire(&inode->i_rwsem);
	err = inode->i_op->mknod(inode, path.dentry, mode, dev);
	sleeplock_release(&inode->i_rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_linkat(int olddirfd, const char *oldpathname, int newdirfd,
	      const char *newpathname, int flags)
{
	(void)flags;

	int err;
	struct path oldpath, newpath;

	err = path_lookupat(olddirfd, oldpathname, 0, &oldpath);
	if (err)
		return err;

	err = path_lookupat(newdirfd, newpathname, 0, &newpath);
	if (err) {
		path_put(&oldpath);
		return err;
	}

	spinlock_acquire(&newpath.dentry->d_lock);
	if (!(newpath.dentry->d_flags & DCACHE_NEGATIVE)) {
		spinlock_release(&newpath.dentry->d_lock);
		path_put(&oldpath);
		path_put(&newpath);
		return -EEXIST;
	}
	spinlock_release(&newpath.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&newpath, &parent_path);
	if (err) {
		path_put(&oldpath);
		path_put(&newpath);
		return err;
	}
	struct inode *parent_inode = parent_path.dentry->d_inode;
	struct inode *old_inode = oldpath.dentry->d_inode;
	sleeplock_acquire(&parent_inode->i_rwsem);
	sleeplock_acquire(&old_inode->i_rwsem);
	err = parent_inode->i_op->link(oldpath.dentry, parent_inode,
				       newpath.dentry);
	sleeplock_release(&old_inode->i_rwsem);
	sleeplock_release(&parent_inode->i_rwsem);

	path_put(&parent_path);
	path_put(&oldpath);
	path_put(&newpath);
	return err;
}

int do_unlinkat(int dirfd, const char *pathname, int flags)
{
	int err;
	struct path path;

	(void)flags;

	err = path_lookupat(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (path.dentry->d_flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->d_inode;
	sleeplock_acquire(&inode->i_rwsem);
	err = inode->i_op->unlink(inode, path.dentry);
	sleeplock_release(&inode->i_rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_symlinkat(int dirfd, const char *pathname, const char *target)
{
	int err;
	struct path path;

	err = path_lookupat(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (!(path.dentry->d_flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->d_inode;
	sleeplock_acquire(&inode->i_rwsem);
	err = inode->i_op->symlink(inode, path.dentry, target);
	sleeplock_release(&inode->i_rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_readlinkat(int dirfd, const char *pathname, char *buf, usize_t bufsiz)
{
	int err;
	struct path path;

	err = path_lookupat(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (path.dentry->d_flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->d_lock);

	struct inode *inode = path.dentry->d_inode;
	if (!S_ISLNK(inode->i_mode)) {
		path_put(&path);
		return -EINVAL;
	}

	if (!inode->i_op->readlink) {
		path_put(&path);
		return -EOPNOTSUPP;
	}

	sleeplock_acquire(&inode->i_rwsem);
	err = inode->i_op->readlink(path.dentry, buf, bufsiz);
	sleeplock_release(&inode->i_rwsem);

	path_put(&path);

	return err;
}

int do_creat(const char *pathname, umode_t mode)
{
	int err;
	struct path path;

	err = path_lookup(pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (!(path.dentry->d_flags & DCACHE_NEGATIVE)) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -EEXIST;
	}
	spinlock_release(&path.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->d_inode;
	sleeplock_acquire(&inode->i_rwsem);
	err = inode->i_op->create(inode, path.dentry, mode, true);
	sleeplock_release(&inode->i_rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_renameat(int olddirfd, const char *oldpathname, int newdirfd,
		const char *newpathname, unsigned int flags)
{
	int err;
	struct path oldpath, newpath;

	err = path_lookupat(olddirfd, oldpathname, 0, &oldpath);
	if (err)
		return err;

	err = path_lookupat(newdirfd, newpathname, 0, &newpath);
	if (err) {
		path_put(&oldpath);
		return err;
	}

	spinlock_acquire(&newpath.dentry->d_lock);
	if (!(newpath.dentry->d_flags & DCACHE_NEGATIVE)) {
		spinlock_release(&newpath.dentry->d_lock);
		path_put(&oldpath);
		path_put(&newpath);
		return -EEXIST;
	}
	spinlock_release(&newpath.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&newpath, &parent_path);
	if (err) {
		path_put(&oldpath);
		path_put(&newpath);
		return err;
	}
	struct inode *parent_inode = parent_path.dentry->d_inode;
	struct inode *old_inode = oldpath.dentry->d_inode;
	sleeplock_acquire(&parent_inode->i_rwsem);
	sleeplock_acquire(&old_inode->i_rwsem);
	err = parent_inode->i_op->rename(old_inode, oldpath.dentry,
					 parent_inode, newpath.dentry, flags);
	sleeplock_release(&old_inode->i_rwsem);
	sleeplock_release(&parent_inode->i_rwsem);

	path_put(&parent_path);
	path_put(&oldpath);
	path_put(&newpath);
	return err;
}

int do_rmdir(const char *pathname)
{
	int err;
	struct path path;

	err = path_lookupat(AT_FDCWD, pathname, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (path.dentry->d_flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->d_lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->d_inode;
	sleeplock_acquire(&inode->i_rwsem);
	err = inode->i_op->rmdir(inode, path.dentry);
	sleeplock_release(&inode->i_rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}
