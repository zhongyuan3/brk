#include <brk/error.h>
#include <brk/fs.h>
#include <brk/path.h>
#include <brk/permission.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>

static umode_t apply_umask(umode_t mode)
{
	return mode & ~current_task()->umask;
}

static int may_create_in(struct fs_inode *dir)
{
	return inode_permission(dir, MAY_WRITE | MAY_EXEC);
}

static int may_delete(struct fs_inode *dir, struct fs_dentry *victim)
{
	struct task *task = current_task();
	struct fs_inode *inode = victim ? victim->inode : NULL;
	int err;

	err = inode_permission(dir, MAY_WRITE | MAY_EXEC);
	if (err)
		return err;

	if (dir->mode & S_ISVTX) {
		if (!capable(CAP_FOWNER) && task->fsuid != dir->uid &&
		    (!inode || task->fsuid != inode->uid))
			return -EPERM;
	}

	return 0;
}

struct fs_file *do_openat(int dirfd, const char *pathname, int flags,
			  umode_t mode)
{
	int err;
	struct fs_file *file;
	struct fs_path path;
	bool retried = false;
	umode_t create_mode = apply_umask(mode);

	if ((flags & O_RDWR) && (flags & O_WRONLY))
		return ERR_PTR(-EINVAL);

retry:
	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return ERR_PTR(err);

	spin_lock(&path.dentry->lock);
	bool exists = !(path.dentry->flags & DCACHE_NEGATIVE);
	spin_unlock(&path.dentry->lock);

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
		err = may_create_in(inode);
		if (err) {
			fs_path_put(&parent_path);
			fs_path_put(&path);
			return ERR_PTR(err);
		}
		sleeplock_acquire(&inode->rwsem);
		err = inode->ops->create(inode, path.dentry, create_mode, excl);
		sleeplock_release(&inode->rwsem);
		fs_path_put(&parent_path);

		if (err == -EEXIST && !excl && !retried) {
			/*
			 * Another CPU created the name after our negative
			 * check.  Redo the lookup once so that path.dentry is
			 * the now-positive dentry, then open it below.
			 */
			fs_path_put(&path);
			retried = true;
			goto retry;
		}
		if (err) {
			fs_path_put(&path);
			return ERR_PTR(err);
		}
	}

	fmode_t fmode = 0;

	if (flags == O_RDONLY)
		fmode = FMODE_READ;

	if (flags & O_WRONLY)
		fmode = FMODE_WRITE;

	if (flags & O_RDWR)
		fmode = FMODE_READ | FMODE_WRITE;

	struct fs_inode *file_inode = fs_dentry_inode(path.dentry);

	if (!file_inode) {
		fs_path_put(&path);
		return ERR_PTR(-ENOENT);
	}

	int acc = flags & O_ACCMODE;
	int perm_mask = 0;

	if (acc != O_WRONLY)
		perm_mask |= MAY_READ;
	if (acc != O_RDONLY)
		perm_mask |= MAY_WRITE;
	if (flags & O_TRUNC)
		perm_mask |= MAY_WRITE;

	err = inode_permission(file_inode, perm_mask);
	if (err) {
		fs_path_put(&path);
		return ERR_PTR(err);
	}

	if (S_ISDIR(file_inode->mode))
		fmode |= FMODE_DIR;

	file = fs_file_alloc(&path, fmode);
	if (IS_ERR(file)) {
		fs_path_put(&path);
		return ERR_CAST(file);
	}

	if (perm_mask & MAY_WRITE)
		fs_file_remove_privs(file);

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

	spin_lock(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spin_unlock(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	err = may_create_in(inode);
	if (err) {
		fs_path_put(&parent_path);
		fs_path_put(&path);
		return err;
	}
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->mkdir(inode, path.dentry,
				apply_umask(mode) | S_IFDIR);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}

int do_mknodat(int dirfd, const char *pathname, umode_t mode, dev_t dev)
{
	int err;
	struct fs_path path;

	if (!capable(CAP_MKNOD))
		return -EPERM;

	err = fs_path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spin_unlock(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	err = may_create_in(inode);
	if (err) {
		fs_path_put(&parent_path);
		fs_path_put(&path);
		return err;
	}
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->mknod(inode, path.dentry,
				apply_umask(mode & 07777) | (mode & S_IFMT),
				dev);
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

	if (fs_dentry_is_negative(oldpath.dentry)) {
		fs_path_put(&oldpath);
		return -ENOENT;
	}

	err = fs_path_lookup_at(newdirfd, newpathname, 0, &newpath);
	if (err) {
		fs_path_put(&oldpath);
		return err;
	}

	spin_lock(&newpath.dentry->lock);
	if (!(newpath.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&newpath.dentry->lock);
		fs_path_put(&oldpath);
		fs_path_put(&newpath);
		return -EEXIST;
	}
	spin_unlock(&newpath.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&newpath, &parent_path);
	if (err) {
		fs_path_put(&oldpath);
		fs_path_put(&newpath);
		return err;
	}
	struct fs_inode *parent_inode = parent_path.dentry->inode;
	struct fs_inode *old_inode = oldpath.dentry->inode;

	/*
	 * Hard-linking a directory is invalid on POSIX systems.  Besides the
	 * semantic error, @old_inode can equal @parent_inode here (linking a
	 * directory below itself), which would make the two rwsem acquires
	 * below self-deadlock before the filesystem has a chance to reject
	 * it.
	 */
	if (S_ISDIR(old_inode->mode) || old_inode == parent_inode) {
		err = -EPERM;
		goto out_paths;
	}

	err = may_create_in(parent_inode);
	if (err)
		goto out_paths;

	if (!inode_owner_or_capable(old_inode)) {
		err = -EPERM;
		goto out_paths;
	}

	sleeplock_acquire(&parent_inode->rwsem);
	sleeplock_acquire(&old_inode->rwsem);
	err = parent_inode->ops->link(oldpath.dentry, parent_inode,
				      newpath.dentry);
	sleeplock_release(&old_inode->rwsem);
	sleeplock_release(&parent_inode->rwsem);

out_paths:
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

	spin_lock(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spin_unlock(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	err = may_delete(inode, path.dentry);
	if (err) {
		fs_path_put(&parent_path);
		fs_path_put(&path);
		return err;
	}
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

	spin_lock(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spin_unlock(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	err = may_create_in(inode);
	if (err) {
		fs_path_put(&parent_path);
		fs_path_put(&path);
		return err;
	}
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

	spin_lock(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spin_unlock(&path.dentry->lock);

	struct fs_inode *inode = fs_dentry_inode(path.dentry);
	if (!inode) {
		fs_path_put(&path);
		return -ENOENT;
	}
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

	spin_lock(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -EEXIST;
	}
	spin_unlock(&path.dentry->lock);

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

static bool rename_dentry_is_ancestor(struct fs_dentry *dentry,
				      struct fs_inode *inode)
{
	while (dentry) {
		if (dentry->inode == inode)
			return true;
		if (!dentry->parent || dentry->parent == dentry)
			break;
		dentry = dentry->parent;
	}
	return false;
}

int do_renameat(int olddirfd, const char *oldpathname, int newdirfd,
		const char *newpathname, unsigned int flags)
{
	struct fs_path oldpath = { 0 };
	struct fs_path newpath = { 0 };
	struct fs_path old_parent = { 0 };
	struct fs_path new_parent = { 0 };
	struct fs_inode *old_inode;
	struct fs_inode *new_inode;
	struct fs_inode *old_dir;
	struct fs_inode *new_dir;
	bool new_exists;
	int err;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;
	if (flags & RENAME_EXCHANGE)
		return -EINVAL;

	err = fs_path_lookup_at(olddirfd, oldpathname, 0, &oldpath);
	if (err)
		return err;

	if (fs_dentry_is_negative(oldpath.dentry)) {
		fs_path_put(&oldpath);
		return -ENOENT;
	}

	err = fs_path_lookup_at(newdirfd, newpathname, 0, &newpath);
	if (err) {
		fs_path_put(&oldpath);
		return err;
	}

	err = fs_path_dot_dot(&oldpath, &old_parent);
	if (err)
		goto out_both;
	err = fs_path_dot_dot(&newpath, &new_parent);
	if (err)
		goto out_old_parent;

	old_inode = oldpath.dentry->inode;
	new_inode = newpath.dentry->inode;
	old_dir = old_parent.dentry->inode;
	new_dir = new_parent.dentry->inode;
	new_exists = new_inode != NULL;

	if (!old_inode || !old_dir || !new_dir ||
	    !S_ISDIR(old_dir->mode) || !S_ISDIR(new_dir->mode)) {
		err = -ENOTDIR;
		goto out;
	}
	if (old_inode->sb != new_dir->sb) {
		err = -EXDEV;
		goto out;
	}
	if (fs_dentry_is_mounted(oldpath.dentry) ||
	    fs_dentry_is_mounted(newpath.dentry)) {
		err = -EBUSY;
		goto out;
	}
	if (old_inode == new_inode) {
		/* Same inode (hard links or identical path): nothing to do. */
		err = 0;
		goto out;
	}
	if (!old_dir->ops || !old_dir->ops->rename) {
		err = -EOPNOTSUPP;
		goto out;
	}

	if (S_ISDIR(old_inode->mode)) {
		if (rename_dentry_is_ancestor(new_parent.dentry, old_inode)) {
			err = -EINVAL;
			goto out;
		}
		/*
		 * There is no d_move() in this dcache yet.  Cross-directory
		 * directory renames would leave child dentries parented to the
		 * old dentry, so only same-directory renames are safe.
		 */
		if (old_dir != new_dir) {
			err = -EXDEV;
			goto out;
		}
	}

	if (new_exists) {
		if (flags & RENAME_NOREPLACE) {
			err = -EEXIST;
			goto out;
		}
		if (S_ISDIR(old_inode->mode) && !S_ISDIR(new_inode->mode)) {
			err = -ENOTDIR;
			goto out;
		}
		if (!S_ISDIR(old_inode->mode) && S_ISDIR(new_inode->mode)) {
			err = -EISDIR;
			goto out;
		}
	}

	err = may_delete(old_dir, oldpath.dentry);
	if (err)
		goto out;
	if (new_exists)
		err = may_delete(new_dir, newpath.dentry);
	else
		err = inode_permission(new_dir, MAY_WRITE | MAY_EXEC);
	if (err)
		goto out;

	/* Lock parent directories in a stable order. */
	if (old_dir == new_dir) {
		sleeplock_acquire(&old_dir->rwsem);
	} else if ((uintptr_t)old_dir < (uintptr_t)new_dir) {
		sleeplock_acquire(&old_dir->rwsem);
		sleeplock_acquire(&new_dir->rwsem);
	} else {
		sleeplock_acquire(&new_dir->rwsem);
		sleeplock_acquire(&old_dir->rwsem);
	}

	err = old_dir->ops->rename(old_dir, oldpath.dentry, new_dir,
				   newpath.dentry, flags);

	if (old_dir != new_dir)
		sleeplock_release(&new_dir->rwsem);
	sleeplock_release(&old_dir->rwsem);

	if (!err) {
		/*
		 * Update the dcache after the filesystem committed the name
		 * change.  grab one reference for the new dentry; making the
		 * old dentry negative drops its own reference.
		 */
		fs_inode_get(old_inode);
		if (new_exists)
			fs_dentry_make_negative(newpath.dentry);
		fs_dentry_make_negative(oldpath.dentry);
		fs_dentry_instantiate(newpath.dentry, old_inode);
	}

out:
	fs_path_put(&new_parent);
out_old_parent:
	fs_path_put(&old_parent);
out_both:
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

	spin_lock(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spin_unlock(&path.dentry->lock);

	struct fs_path parent_path;
	err = fs_path_dot_dot(&path, &parent_path);
	if (err) {
		fs_path_put(&path);
		return err;
	}
	struct fs_inode *inode = parent_path.dentry->inode;
	err = may_delete(inode, path.dentry);
	if (err) {
		fs_path_put(&parent_path);
		fs_path_put(&path);
		return err;
	}
	sleeplock_acquire(&inode->rwsem);
	err = inode->ops->rmdir(inode, path.dentry);
	sleeplock_release(&inode->rwsem);

	fs_path_put(&parent_path);
	fs_path_put(&path);
	return err;
}
