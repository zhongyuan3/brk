#include <brk/error.h>
#include <brk/fs.h>
#include <brk/path.h>
#include <brk/permission.h>
#include <brk/rwsem.h>
#include <brk/spinlock.h>
#include <brk/task.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

static umode_t apply_umask(umode_t mode)
{
	return mode & ~process_get_umask(current_process());
}

static int may_create_in(struct inode *dir)
{
	return inode_permission(dir, MAY_WRITE | MAY_EXEC);
}

static int may_delete(struct inode *dir, struct dentry *victim)
{
	struct inode *inode = victim ? victim->inode : NULL;
	kuid_t fsuid = process_get_fsuid(current_process());
	int err;

	err = inode_permission(dir, MAY_WRITE | MAY_EXEC);
	if (err)
		return err;

	if (dir->mode & S_ISVTX) {
		if (!capable(CAP_FOWNER) && fsuid != dir->uid &&
		    (!inode || fsuid != inode->uid))
			return -EPERM;
	}

	return 0;
}

struct file *do_openat(int dirfd, const char *pathname, int flags, umode_t mode)
{
	int err;
	struct file *file;
	struct path path;
	bool retried = false;
	umode_t create_mode = apply_umask(mode);
	unsigned int lookup_flags;

	if (flags & O_NOFOLLOW)
		lookup_flags = 0;
	else if ((flags & O_CREAT) && (flags & O_EXCL))
		lookup_flags = 0;
	else
		lookup_flags = LOOKUP_FOLLOW;

	if ((flags & O_RDWR) && (flags & O_WRONLY))
		return ERR_PTR(-EINVAL);

retry:
	err = path_lookup_at(dirfd, pathname, lookup_flags, &path);
	if (err)
		return ERR_PTR(err);

	if ((flags & O_NOFOLLOW) && !((flags & O_CREAT) && (flags & O_EXCL))) {
		struct inode *link_inode = dentry_inode(path.dentry);

		if (link_inode && S_ISLNK(link_inode->mode)) {
			path_put(&path);
			return ERR_PTR(-ELOOP);
		}
	}

	spin_lock(&path.dentry->lock);
	bool exists = !(path.dentry->flags & DCACHE_NEGATIVE);
	spin_unlock(&path.dentry->lock);

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
		struct inode *inode = parent_path.dentry->inode;
		err = may_create_in(inode);
		if (err) {
			path_put(&parent_path);
			path_put(&path);
			return ERR_PTR(err);
		}
		down_write(&inode->rwsem);
		err = inode->ops->create(inode, path.dentry, create_mode, excl);
		up_write(&inode->rwsem);
		path_put(&parent_path);

		if (err == -EEXIST && !excl && !retried) {
			/*
			 * Another CPU created the name after our negative
			 * check.  Redo the lookup once so that path.dentry is
			 * the now-positive dentry, then open it below.
			 */
			path_put(&path);
			retried = true;
			goto retry;
		}
		if (err) {
			path_put(&path);
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

	struct inode *file_inode = dentry_inode(path.dentry);

	if (!file_inode) {
		path_put(&path);
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
		path_put(&path);
		return ERR_PTR(err);
	}

	if (S_ISDIR(file_inode->mode))
		fmode |= FMODE_DIR;

	file = file_alloc(&path, fmode);
	if (IS_ERR(file)) {
		path_put(&path);
		return ERR_CAST(file);
	}

	if (perm_mask & MAY_WRITE)
		file_remove_privs(file);

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

	err = path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&path.dentry->lock);
		path_put(&path);
		return -EEXIST;
	}
	spin_unlock(&path.dentry->lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->inode;
	err = may_create_in(inode);
	if (err) {
		path_put(&parent_path);
		path_put(&path);
		return err;
	}
	down_write(&inode->rwsem);
	err = inode->ops->mkdir(inode, path.dentry,
				apply_umask(mode) | S_IFDIR);
	up_write(&inode->rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_mknodat(int dirfd, const char *pathname, umode_t mode, dev_t dev)
{
	int err;
	struct path path;

	if (!capable(CAP_MKNOD))
		return -EPERM;

	err = path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&path.dentry->lock);
		path_put(&path);
		return -EEXIST;
	}
	spin_unlock(&path.dentry->lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->inode;
	err = may_create_in(inode);
	if (err) {
		path_put(&parent_path);
		path_put(&path);
		return err;
	}
	down_write(&inode->rwsem);
	err = inode->ops->mknod(inode, path.dentry,
				apply_umask(mode & 07777) | (mode & S_IFMT),
				dev);
	up_write(&inode->rwsem);

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

	err = path_lookup_at(olddirfd, oldpathname, 0, &oldpath);
	if (err)
		return err;

	if (dentry_is_negative(oldpath.dentry)) {
		path_put(&oldpath);
		return -ENOENT;
	}

	err = path_lookup_at(newdirfd, newpathname, 0, &newpath);
	if (err) {
		path_put(&oldpath);
		return err;
	}

	spin_lock(&newpath.dentry->lock);
	if (!(newpath.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&newpath.dentry->lock);
		path_put(&oldpath);
		path_put(&newpath);
		return -EEXIST;
	}
	spin_unlock(&newpath.dentry->lock);

	struct path parent_path;
	err = path_dot_dot(&newpath, &parent_path);
	if (err) {
		path_put(&oldpath);
		path_put(&newpath);
		return err;
	}
	struct inode *parent_inode = parent_path.dentry->inode;
	struct inode *old_inode = oldpath.dentry->inode;

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

	down_write(&parent_inode->rwsem);
	down_write(&old_inode->rwsem);
	err = parent_inode->ops->link(oldpath.dentry, parent_inode,
				      newpath.dentry);
	up_write(&old_inode->rwsem);
	up_write(&parent_inode->rwsem);

out_paths:
	path_put(&parent_path);
	path_put(&oldpath);
	path_put(&newpath);
	return err;
}

static int do_rmdirat(int dirfd, const char *pathname)
{
	int err;
	struct path path;

	err = path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spin_unlock(&path.dentry->lock);
		path_put(&path);
		return -ENOENT;
	}
	spin_unlock(&path.dentry->lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->inode;
	err = may_delete(inode, path.dentry);
	if (err) {
		path_put(&parent_path);
		path_put(&path);
		return err;
	}
	down_write(&inode->rwsem);
	err = inode->ops->rmdir(inode, path.dentry);
	up_write(&inode->rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_unlinkat(int dirfd, const char *pathname, int flags)
{
	int err;
	struct path path;

	if (flags & ~AT_REMOVEDIR)
		return -EINVAL;
	if (flags & AT_REMOVEDIR)
		return do_rmdirat(dirfd, pathname);

	err = path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spin_unlock(&path.dentry->lock);
		path_put(&path);
		return -ENOENT;
	}
	spin_unlock(&path.dentry->lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->inode;
	err = may_delete(inode, path.dentry);
	if (err) {
		path_put(&parent_path);
		path_put(&path);
		return err;
	}
	down_write(&inode->rwsem);
	err = inode->ops->unlink(inode, path.dentry);
	up_write(&inode->rwsem);

	if (!err)
		dentry_make_negative(path.dentry);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_symlinkat(int dirfd, const char *pathname, const char *target)
{
	int err;
	struct path path;

	err = path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (!(path.dentry->flags & DCACHE_NEGATIVE)) {
		spin_unlock(&path.dentry->lock);
		path_put(&path);
		return -EEXIST;
	}
	spin_unlock(&path.dentry->lock);

	struct path parent_path;
	err = path_dot_dot(&path, &parent_path);
	if (err) {
		path_put(&path);
		return err;
	}
	struct inode *inode = parent_path.dentry->inode;
	err = may_create_in(inode);
	if (err) {
		path_put(&parent_path);
		path_put(&path);
		return err;
	}
	down_write(&inode->rwsem);
	err = inode->ops->symlink(inode, path.dentry, target);
	up_write(&inode->rwsem);

	path_put(&parent_path);
	path_put(&path);
	return err;
}

int do_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz)
{
	int err;
	struct path path;

	err = path_lookup_at(dirfd, pathname, 0, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spin_unlock(&path.dentry->lock);
		path_put(&path);
		return -ENOENT;
	}
	spin_unlock(&path.dentry->lock);

	struct inode *inode = dentry_inode(path.dentry);
	if (!inode) {
		path_put(&path);
		return -ENOENT;
	}
	if (!S_ISLNK(inode->mode)) {
		path_put(&path);
		return -EINVAL;
	}

	if (!inode->ops->readlink) {
		path_put(&path);
		return -EOPNOTSUPP;
	}

	down_read(&inode->rwsem);
	err = inode->ops->readlink(path.dentry, buf, bufsiz);
	up_read(&inode->rwsem);

	path_put(&path);

	return err;
}

static bool rename_dentry_is_ancestor(struct dentry *dentry,
				      struct inode *inode)
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
	struct path oldpath = { 0 };
	struct path newpath = { 0 };
	struct path old_parent = { 0 };
	struct path new_parent = { 0 };
	struct inode *old_inode;
	struct inode *new_inode;
	struct inode *old_dir;
	struct inode *new_dir;
	bool new_exists;
	int err;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;
	if (flags & RENAME_EXCHANGE)
		return -EINVAL;

	err = path_lookup_at(olddirfd, oldpathname, 0, &oldpath);
	if (err)
		return err;

	if (dentry_is_negative(oldpath.dentry)) {
		path_put(&oldpath);
		return -ENOENT;
	}

	err = path_lookup_at(newdirfd, newpathname, 0, &newpath);
	if (err) {
		path_put(&oldpath);
		return err;
	}

	err = path_dot_dot(&oldpath, &old_parent);
	if (err)
		goto out_both;
	err = path_dot_dot(&newpath, &new_parent);
	if (err)
		goto out_old_parent;

	old_inode = oldpath.dentry->inode;
	new_inode = newpath.dentry->inode;
	old_dir = old_parent.dentry->inode;
	new_dir = new_parent.dentry->inode;
	new_exists = new_inode != NULL;

	if (!old_inode || !old_dir || !new_dir || !S_ISDIR(old_dir->mode) ||
	    !S_ISDIR(new_dir->mode)) {
		err = -ENOTDIR;
		goto out;
	}
	if (old_inode->sb != new_dir->sb) {
		err = -EXDEV;
		goto out;
	}
	if (dentry_is_mounted(oldpath.dentry) ||
	    dentry_is_mounted(newpath.dentry)) {
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
		down_write(&old_dir->rwsem);
	} else if ((uintptr_t)old_dir < (uintptr_t)new_dir) {
		down_write(&old_dir->rwsem);
		down_write(&new_dir->rwsem);
	} else {
		down_write(&new_dir->rwsem);
		down_write(&old_dir->rwsem);
	}

	err = old_dir->ops->rename(old_dir, oldpath.dentry, new_dir,
				   newpath.dentry, flags);

	if (old_dir != new_dir)
		up_write(&new_dir->rwsem);
	up_write(&old_dir->rwsem);

	if (!err) {
		/*
		 * Update the dcache after the filesystem committed the name
		 * change.  grab one reference for the new dentry; making the
		 * old dentry negative drops its own reference.
		 */
		inode_get(old_inode);
		if (new_exists)
			dentry_make_negative(newpath.dentry);
		dentry_make_negative(oldpath.dentry);
		dentry_instantiate(newpath.dentry, old_inode);
	}

out:
	path_put(&new_parent);
out_old_parent:
	path_put(&old_parent);
out_both:
	path_put(&oldpath);
	path_put(&newpath);
	return err;
}
