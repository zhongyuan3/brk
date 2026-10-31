#include <brk/cpu.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/fs_types.h>
#include <brk/fsinfo.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/permission.h>
#include <brk/printk.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/dirent.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/limits.h>
#include <uapi/stat.h>
#include <uapi/time.h>
#include <uapi/types.h>
#include <uapi/utsname.h>

u64 sys_read(void)
{
	int fd;
	void *buf;
	size_t n;
	struct fs_file *fp;
	ssize_t ret;
	struct task *task;

	task = current_task();
	fd = syscall_arg_int(0);
	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	buf = syscall_arg_ptr(1);
	n = syscall_arg_raw(2);
	ret = fs_file_read(fp, buf, n);
	fs_file_put(fp);
	return ret;
}

u64 sys_write(void)
{
	int fd;
	const void *buf;
	size_t n;
	struct fs_file *fp;
	ssize_t ret;
	struct task *task;

	task = current_task();
	fd = syscall_arg_int(0);
	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	if (fp->mode & FMODE_DIR) {
		fs_file_put(fp);
		return -EISDIR;
	}
	buf = syscall_arg_ptr(1);
	n = syscall_arg_raw(2);
	ret = fs_file_write(fp, buf, n);
	fs_file_put(fp);
	return ret;
}

u64 sys_open(void)
{
	int fd;
	struct fs_file *fp = NULL;
	char *path = syscall_arg_ptr(0);
	int flags = syscall_arg_raw(1);
	umode_t mode = syscall_arg_raw(2);

	fp = do_openat(AT_FDCWD, path, flags, mode);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	fd = fdtable_alloc_fd(current_task()->fdtable, fp);
	if (fd < 0) {
		fs_file_put(fp);
		return -EMFILE;
	}
	return fd;
}

u64 sys_openat(void)
{
	int fd;
	struct fs_file *fp = NULL;
	int dirfd = syscall_arg_raw(0);
	char *path = syscall_arg_ptr(1);
	int flags = syscall_arg_raw(2);
	umode_t mode = syscall_arg_raw(3);

	fp = do_openat(dirfd, path, flags, mode);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	fd = fdtable_alloc_fd(current_task()->fdtable, fp);
	if (fd < 0) {
		fs_file_put(fp);
		return -EMFILE;
	}
	return fd;
}

u64 sys_close(void)
{
	int fd = syscall_arg_int(0);
	struct task *task = current_task();
	return fdtable_close_fd(task->fdtable, fd);
}

u64 sys_execve(void)
{
	char *path = syscall_arg_ptr(0);
	char **argv = syscall_arg_ptr(1);
	char **envp = syscall_arg_ptr(2);
	return do_execve(path, argv, envp);
}






u64 sys_fstat(void)
{
	struct fs_file *fp = NULL;
	struct stat *buf;
	int ret;
	int fd;
	struct task *task = current_task();

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	buf = syscall_arg_ptr(1);
	ret = fs_file_stat(fp, buf);
	fs_file_put(fp);
	return ret;
}

static u64 stat_path(const char *path, struct stat *buf, unsigned int flags)
{
	struct fs_path p;
	struct fs_inode *inode;
	int ret;

	ret = fs_path_lookup(path, flags, &p);
	if (ret)
		return ret;

	inode = p.dentry->inode;
	if (!inode) {
		fs_path_put(&p);
		return -ENOENT;
	}
	if (!inode->ops || !inode->ops->getattr) {
		fs_path_put(&p);
		return -EOPNOTSUPP;
	}

	ret = inode->ops->getattr(&p, buf, 0, 0);
	fs_path_put(&p);
	return ret;
}

u64 sys_lstat(void)
{
	const char *path = syscall_arg_ptr(0);
	struct stat *buf = syscall_arg_ptr(1);

	return stat_path(path, buf, 0);
}

u64 sys_stat(void)
{
	const char *path = syscall_arg_ptr(0);
	struct stat *buf = syscall_arg_ptr(1);

	return stat_path(path, buf, LOOKUP_FOLLOW);
}

static u64 access_path_at(int dirfd, const char *path, int mode,
			  unsigned int lookup_flags)
{
	struct fs_path p;
	struct fs_inode *inode;
	int ret;

	if (mode & ~7)
		return -EINVAL;

	ret = fs_path_lookup_at(dirfd, path, lookup_flags, &p);
	if (ret)
		return ret;

	inode = p.dentry->inode;
	if (!inode) {
		fs_path_put(&p);
		return -ENOENT;
	}

	ret = inode_permission(inode, mode & 7);
	fs_path_put(&p);
	return ret;
}

u64 sys_access(void)
{
	const char *path = syscall_arg_ptr(0);
	int mode = syscall_arg_int(1);

	return access_path_at(AT_FDCWD, path, mode, LOOKUP_FOLLOW);
}

u64 sys_faccessat(void)
{
	int dirfd = syscall_arg_int(0);
	const char *path = syscall_arg_ptr(1);
	int mode = syscall_arg_int(2);

	return access_path_at(dirfd, path, mode, LOOKUP_FOLLOW);
}

u64 sys_faccessat2(void)
{
	int dirfd = syscall_arg_int(0);
	const char *path = syscall_arg_ptr(1);
	int mode = syscall_arg_int(2);
	int flags = syscall_arg_int(3);
	unsigned int lookup_flags;

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS))
		return -EINVAL;

	lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	return access_path_at(dirfd, path, mode, lookup_flags);
}

u64 sys_fstatfs(void)
{
	int fd = syscall_arg_int(0);
	struct statfs *buf = syscall_arg_ptr(1);
	struct task *task = current_task();
	struct fs_file *fp;
	int ret;

	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	ret = fs_statfs(fp->inode->sb, buf);
	fs_file_put(fp);
	return ret;
}

u64 sys_statfs(void)
{
	const char *path = syscall_arg_ptr(0);
	struct statfs *buf = syscall_arg_ptr(1);
	struct fs_path p = { 0 };
	struct fs_inode *inode;
	int ret;

	ret = fs_path_lookup(path, LOOKUP_FOLLOW, &p);
	if (ret)
		return ret;

	inode = p.dentry ? p.dentry->inode : NULL;
	if (!inode) {
		fs_path_put(&p);
		return -ENOENT;
	}

	ret = fs_statfs(inode->sb, buf);
	fs_path_put(&p);
	return ret;
}

u64 sys_fallocate(void)
{
	int fd = syscall_arg_int(0);
	int mode = syscall_arg_int(1);
	loff_t offset = (loff_t)syscall_arg_raw(2);
	loff_t len = (loff_t)syscall_arg_raw(3);
	struct task *task = current_task();
	struct fs_file *fp;
	int ret;

	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_WRITE)) {
		fs_file_put(fp);
		return -EINVAL;
	}

	ret = fs_file_fallocate(fp, mode, offset, len);
	fs_file_put(fp);
	return ret;
}

u64 sys_link(void)
{
	char *oldpath = syscall_arg_ptr(0);
	char *newpath = syscall_arg_ptr(1);
	return do_linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

u64 sys_linkat(void)
{
	int olddirfd = syscall_arg_int(0);
	char *oldpath = syscall_arg_ptr(1);
	int newdirfd = syscall_arg_int(2);
	char *newpath = syscall_arg_ptr(3);
	int flags = syscall_arg_raw(4);
	return do_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}

u64 sys_unlink(void)
{
	char *path = syscall_arg_ptr(0);
	return do_unlinkat(AT_FDCWD, path, 0);
}

u64 sys_unlinkat(void)
{
	int dirfd = syscall_arg_int(0);
	char *path = syscall_arg_ptr(1);
	int flags = syscall_arg_int(2);
	return do_unlinkat(dirfd, path, flags);
}

u64 sys_symlink(void)
{
	const char *target = syscall_arg_ptr(0);
	char *linkpath = syscall_arg_ptr(1);
	return do_symlinkat(AT_FDCWD, linkpath, target);
}

u64 sys_readlink(void)
{
	const char *pathname = syscall_arg_ptr(0);
	char *buf = syscall_arg_ptr(1);
	size_t bufsiz = syscall_arg_raw(2);
	return do_readlinkat(AT_FDCWD, pathname, buf, bufsiz);
}

u64 sys_rename(void)
{
	const char *oldpathname = syscall_arg_ptr(0);
	const char *newpathname = syscall_arg_ptr(1);
	return do_renameat(AT_FDCWD, oldpathname, AT_FDCWD, newpathname, 0);
}

u64 sys_creat(void)
{
	const char *pathname = syscall_arg_ptr(0);
	umode_t mode = syscall_arg_raw(1);
	return do_creat(pathname, mode);
}

u64 sys_rmdir(void)
{
	const char *pathname = syscall_arg_ptr(0);
	return do_rmdir(pathname);
}

u64 sys_uname(void)
{
	struct utsname name = {
		.sysname = "BRK",
		.nodename = "none",
		.release = BRK_VERSION,
		.version = BRK_VERSION,
		.domainname = "none",
	};
	struct utsname *buf = syscall_arg_ptr(0);

	if (!buf)
		return -EFAULT;

	strlcpy(name.machine, uname_machine(), sizeof(name.machine));
	*buf = name;
	return 0;
}

u64 sys_getcwd(void)
{
	struct task *task = current_task();
	char *buf = syscall_arg_ptr(0);
	size_t size = syscall_arg_raw(1);
	struct fs_path cwd = { 0 };
	fsinfo_get_cwd(task->fsinfo, &cwd);
	int ret = fs_path_to_absolute(&cwd, buf, size);
	fs_path_put(&cwd);
	return ret;
}

u64 sys_chdir(void)
{
	char *pathname = syscall_arg_ptr(0);
	struct task *task = current_task();
	struct fs_path new_path;

	int err = fs_path_lookup(pathname, LOOKUP_FOLLOW, &new_path);
	if (err)
		return err;

	struct fs_inode *inode = fs_dentry_inode(new_path.dentry);

	if (!inode) {
		fs_path_put(&new_path);
		return -ENOENT;
	}

	if (!S_ISDIR(inode->mode)) {
		fs_path_put(&new_path);
		return -ENOTDIR;
	}

	int perm = inode_permission(inode, MAY_EXEC);
	if (perm) {
		fs_path_put(&new_path);
		return perm;
	}

	fsinfo_update_cwd(task->fsinfo, &new_path);
	return 0;
}

u64 sys_fchdir(void)
{
	int fd;
	struct fs_file *fp;
	struct task *task = current_task();

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_DIR)) {
		fs_file_put(fp);
		return -ENOTDIR;
	}

	int perm = inode_permission(fp->inode, MAY_EXEC);
	if (perm) {
		fs_file_put(fp);
		return perm;
	}

	fs_path_get(&fp->path);
	fsinfo_update_cwd(task->fsinfo, &fp->path);
	fs_file_put(fp);
	return 0;
}

u64 sys_renameat(void)
{
	int olddirfd = syscall_arg_int(0);
	const char *oldpathname = syscall_arg_ptr(1);
	int newdirfd = syscall_arg_int(2);
	const char *newpathname = syscall_arg_ptr(3);
	return do_renameat(olddirfd, oldpathname, newdirfd, newpathname, 0);
}

u64 sys_renameat2(void)
{
	int olddirfd = syscall_arg_int(0);
	const char *oldpathname = syscall_arg_ptr(1);
	int newdirfd = syscall_arg_int(2);
	const char *newpathname = syscall_arg_ptr(3);
	int flags = syscall_arg_raw(4);
	return do_renameat(olddirfd, oldpathname, newdirfd, newpathname, flags);
}

u64 sys_symlinkat(void)
{
	const char *target;
	int newdirfd;
	const char *linkpath;

	target = syscall_arg_ptr(0);
	newdirfd = syscall_arg_int(1);
	linkpath = syscall_arg_ptr(2);
	return do_symlinkat(newdirfd, linkpath, target);
}

u64 sys_readlinkat(void)
{
	int dirfd;
	const char *pathname;
	char *buf;
	size_t bufsiz;

	dirfd = syscall_arg_int(0);
	pathname = syscall_arg_ptr(1);
	buf = syscall_arg_ptr(2);
	bufsiz = syscall_arg_raw(3);
	return do_readlinkat(dirfd, pathname, buf, bufsiz);
}

u64 sys_mkdir(void)
{
	char *path = syscall_arg_ptr(0);
	umode_t mode = syscall_arg_raw(1);

	return do_mkdirat(AT_FDCWD, path, mode);
}

u64 sys_mkdirat(void)
{
	int dirfd = syscall_arg_int(0);
	char *pathname = syscall_arg_ptr(1);
	umode_t mode = syscall_arg_raw(2);

	return do_mkdirat(dirfd, pathname, mode);
}

u64 sys_mknod(void)
{
	char *path = syscall_arg_ptr(0);
	umode_t mode = syscall_arg_raw(1);
	dev_t dev = syscall_arg_raw(2);
	return do_mknodat(AT_FDCWD, path, mode, dev);
}

u64 sys_mknodat(void)
{
	int dirfd = syscall_arg_int(0);
	char *path = syscall_arg_ptr(1);
	umode_t mode = syscall_arg_raw(2);
	dev_t dev = syscall_arg_raw(3);
	return do_mknodat(dirfd, path, mode, dev);
}

u64 sys_pipe(void)
{
	int *pipefd = syscall_arg_ptr(0);
	int err;

	if (!pipefd)
		return -EFAULT;

	err = do_pipe2(pipefd, 0);
	return err < 0 ? (u64)(long)err : 0;
}

u64 sys_pipe2(void)
{
	int *pipefd = syscall_arg_ptr(0);
	int flags = syscall_arg_int(1);

	if (!pipefd)
		return -EFAULT;

	return (u64)(long)do_pipe2(pipefd, flags);
}

u64 sys_dup(void)
{
	int oldfd = syscall_arg_int(0);
	struct task *task = current_task();
	return fdtable_dup_fd(task->fdtable, oldfd);
}

u64 sys_dup2(void)
{
	int oldfd = syscall_arg_int(0);
	int newfd = syscall_arg_int(1);
	struct task *task = current_task();
	int err = fdtable_dup_fd2(task->fdtable, oldfd, newfd);
	if (err)
		return err;
	return newfd;
}

u64 sys_mount(void)
{
	const char *source = syscall_arg_ptr(0);
	const char *target = syscall_arg_ptr(1);
	const char *type = syscall_arg_ptr(2);
	unsigned long flags = syscall_arg_raw(3);
	void *data = syscall_arg_ptr(4);

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return do_mount(source, target, type, flags, data);
}

u64 sys_umount2(void)
{
	const char *target = syscall_arg_ptr(0);
	int flags = syscall_arg_int(1);
	struct fs_path path;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	/*
	 * Resolve the mountpoint as seen from its parent mount.  With plain
	 * flags the final follow_mount() would enter the filesystem we are
	 * trying to unmount, and fs_mount_state_lookup() would look for a
	 * nested mount instead of this one.
	 */
	err = fs_path_lookup(target, LOOKUP_MOUNTPOINT, &path);
	if (err)
		return err;

	spin_lock(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spin_unlock(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spin_unlock(&path.dentry->lock);

	struct fs_mount_state *mnt = fs_mount_state_lookup(&path);
	if (!mnt) {
		fs_path_put(&path);
		return -ENOENT;
	}

	err = do_umount(mnt, flags);
	if (err) {
		fs_mount_state_put(mnt);
		fs_path_put(&path);
		return err;
	}

	/*
	 * do_umount() dropped the topology reference; the reference obtained
	 * by fs_mount_state_lookup() above belongs to this syscall.
	 */
	fs_mount_state_put(mnt);
	fs_path_put(&path);
	return 0;
}

u64 sys_lseek(void)
{
	int fd = 0;
	struct fs_file *fp = NULL;
	int ret;
	off_t off;
	int whence;

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	off = syscall_arg_raw(1);
	whence = syscall_arg_int(2);

	ret = fs_file_lseek(fp, off, whence);
	fs_file_put(fp);
	return ret;
}

struct getdents_context {
	struct fs_dir_iterator ctx;
	u8 *buf;
	size_t size;
	size_t pos;
};

static bool getdents_filldir(struct fs_dir_iterator *ctx, const char *name,
			     int namelen, loff_t offset, u64 ino,
			     unsigned int d_type)
{
	struct getdents_context *gctx;
	struct dirent *de;

	gctx = container_of(ctx, struct getdents_context, ctx);
	if (gctx->pos > gctx->size)
		return false;

	size_t len = offsetof(struct dirent, d_name) + namelen + 1;
	len = round_up(len, alignof(struct dirent));

	if (len > gctx->size - gctx->pos)
		return false;

	de = (struct dirent *)(gctx->buf + gctx->pos);
	de->d_ino = ino;
	de->d_off = offset;
	de->d_reclen = len;
	de->d_type = d_type;
	memcpy(de->d_name, name, namelen);
	de->d_name[namelen] = '\0';
	gctx->pos += len;

	return true;
}

u64 sys_getdents(void)
{
	int fd = -1;
	struct fs_file *fp = NULL;
	void *buf;
	size_t size;
	int err;

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_DIR)) {
		fs_file_put(fp);
		return -ENOTDIR;
	}

	buf = syscall_arg_ptr(1);
	size = syscall_arg_raw(2);

	struct fs_inode *inode = fs_dentry_inode(fp->path.dentry);
	const struct fs_file_ops *fop = fp->ops;

	if (!inode) {
		fs_file_put(fp);
		return -ENOENT;
	}
	if (!fop->iterate_shared) {
		fs_file_put(fp);
		return -EOPNOTSUPP;
	}

	sleeplock_acquire(&fp->pos_lock);

	struct getdents_context ctx = {
		.ctx.actor = getdents_filldir,
		.ctx.pos = fp->pos,
		.buf = buf,
		.size = size,
		.pos = 0,
	};

	sleeplock_acquire(&inode->rwsem);
	err = fop->iterate_shared(fp, &ctx.ctx);
	sleeplock_release(&inode->rwsem);
	if (err) {
		sleeplock_release(&fp->pos_lock);
		fs_file_put(fp);
		return err;
	}

	fp->pos = ctx.pos;
	sleeplock_release(&fp->pos_lock);
	fs_file_put(fp);

	return ctx.pos;
}

struct getdents64_context {
	struct fs_dir_iterator ctx;
	u8 *buf;
	size_t size;
	size_t pos;
};

static bool getdents64_filldir(struct fs_dir_iterator *ctx, const char *name,
			       int namelen, loff_t offset, u64 ino,
			       unsigned int d_type)
{
	struct getdents64_context *gctx;
	struct dirent64 *de;

	gctx = container_of(ctx, struct getdents64_context, ctx);
	if (gctx->pos > gctx->size)
		return false;

	size_t len = offsetof(struct dirent64, d_name) + namelen + 1;
	len = round_up(len, alignof(struct dirent64));

	if (len > gctx->size - gctx->pos)
		return false;

	de = (struct dirent64 *)(gctx->buf + gctx->pos);
	de->d_ino = ino;
	de->d_off = offset;
	de->d_reclen = len;
	de->d_type = d_type;
	memcpy(de->d_name, name, namelen);
	de->d_name[namelen] = '\0';
	gctx->pos += len;

	return true;
}

u64 sys_getdents64(void)
{
	int fd = -1;
	struct fs_file *fp = NULL;
	void *buf;
	size_t size;
	int err;

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_DIR)) {
		pr_info("%s(): Not a directory\n", __func__);
		fs_file_put(fp);
		return -ENOTDIR;
	}

	buf = syscall_arg_ptr(1);
	size = syscall_arg_raw(2);

	struct fs_inode *inode = fs_dentry_inode(fp->path.dentry);
	const struct fs_file_ops *fop = fp->ops;

	if (!inode) {
		fs_file_put(fp);
		return -ENOENT;
	}
	if (!fop->iterate_shared) {
		fs_file_put(fp);
		return -EOPNOTSUPP;
	}

	sleeplock_acquire(&fp->pos_lock);

	struct getdents64_context ctx = {
		.ctx.actor = getdents64_filldir,
		.ctx.pos = fp->pos,
		.buf = buf,
		.size = size,
		.pos = 0,
	};

	sleeplock_acquire(&inode->rwsem);
	err = fop->iterate_shared(fp, &ctx.ctx);
	sleeplock_release(&inode->rwsem);
	if (err) {
		sleeplock_release(&fp->pos_lock);
		fs_file_put(fp);
		return err;
	}

	fp->pos = ctx.pos;
	sleeplock_release(&fp->pos_lock);
	fs_file_put(fp);

	return ctx.pos;
}

u64 sys_ioctl(void)
{
	struct fs_file *fp;
	int fd;

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	unsigned int cmd = (unsigned int)syscall_arg_raw(1);
	unsigned long arg = (unsigned long)syscall_arg_raw(2);
	long ret = fs_file_ioctl(fp, cmd, arg);
	fs_file_put(fp);
	return (u64)(long)ret;
}

u64 sys_ftruncate(void)
{
	int fd = syscall_arg_int(0);
	loff_t length = (loff_t)syscall_arg_raw(1);
	struct fs_file *fp;
	int ret;

	if (length < 0)
		return -EINVAL;

	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_WRITE)) {
		fs_file_put(fp);
		return -EINVAL;
	}

	ret = fs_file_truncate(fp, length);
	fs_file_put(fp);
	return ret;
}

u64 sys_truncate(void)
{
	const char *pathname = syscall_arg_ptr(0);
	loff_t length = (loff_t)syscall_arg_raw(1);
	struct fs_iattr attr = {
		.valid = ATTR_SIZE,
		.size = length,
	};
	struct fs_path path;
	struct fs_inode *inode;
	int err;

	if (length < 0)
		return -EINVAL;

	err = fs_path_lookup(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = path.dentry->inode;
	if (!inode) {
		fs_path_put(&path);
		return -ENOENT;
	}
	if (!S_ISREG(inode->mode)) {
		fs_path_put(&path);
		return S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
	}
	if (!inode->ops || !inode->ops->setattr) {
		fs_path_put(&path);
		return -EOPNOTSUPP;
	}

	int perm = inode_permission(inode, MAY_WRITE);
	if (perm) {
		fs_path_put(&path);
		return perm;
	}

	fs_remove_inode_privs(inode, path.dentry);

	err = inode->ops->setattr(path.dentry, &attr);
	fs_path_put(&path);
	return err;
}

static int check_setattr_perm(struct fs_inode *inode, struct fs_iattr *attr)
{
	if (!inode)
		return -ENOENT;

	if ((attr->valid & (ATTR_UID | ATTR_GID)) && !capable(CAP_CHOWN))
		return -EPERM;

	if (attr->valid & (ATTR_MODE | ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)) {
		if (!inode_owner_or_capable(inode))
			return -EPERM;
	}

	return 0;
}

static int setattr_path_at(int dirfd, const char *path, struct fs_iattr *attr,
			   unsigned int lookup_flags)
{
	struct fs_path p;
	struct fs_inode *inode;
	int err;

	if (!path)
		return -EINVAL;

	err = fs_path_lookup_at(dirfd, path, lookup_flags, &p);
	if (err)
		return err;

	inode = p.dentry->inode;
	if (!inode) {
		err = -ENOENT;
		goto out;
	}
	if (!inode->ops || !inode->ops->setattr) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = check_setattr_perm(inode, attr);
	if (err)
		goto out;

	err = inode->ops->setattr(p.dentry, attr);

out:
	fs_path_put(&p);
	return err;
}

static int setattr_fd(int fd, struct fs_iattr *attr)
{
	struct fs_file *fp;
	int err;

	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!fp->inode) {
		err = -EINVAL;
		goto out;
	}
	if (!fp->inode->ops || !fp->inode->ops->setattr) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = check_setattr_perm(fp->inode, attr);
	if (err)
		goto out;

	err = fp->inode->ops->setattr(fp->path.dentry, attr);

out:
	fs_file_put(fp);
	return err;
}

static int times_from_user_timespec(struct fs_iattr *attr,
				    const struct timespec times[2])
{
	struct timespec now;
	struct timespec ts;

	if (!times) {
		ktime_get_real_ts(&now);
		attr->valid |= ATTR_ATIME | ATTR_MTIME;
		attr->atime = now;
		attr->mtime = now;
		return 0;
	}

	for (int i = 0; i < 2; i++) {
		ts = times[i];
		if (ts.tv_nsec == UTIME_OMIT)
			continue;
		if (ts.tv_nsec == UTIME_NOW) {
			ktime_get_real_ts(&now);
			ts = now;
		} else if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
			return -EINVAL;
		}

		if (i == 0) {
			attr->atime = ts;
			attr->valid |= ATTR_ATIME;
		} else {
			attr->mtime = ts;
			attr->valid |= ATTR_MTIME;
		}
	}
	return 0;
}

static int set_times_path_at(int dirfd, const char *path,
			     const struct timespec times[2],
			     unsigned int lookup_flags)
{
	struct fs_iattr attr = { 0 };
	int err;

	err = times_from_user_timespec(&attr, times);
	if (err || !attr.valid)
		return err;

	return setattr_path_at(dirfd, path, &attr, lookup_flags);
}

static int set_times_fd(int fd, const struct timespec times[2])
{
	struct fs_iattr attr = { 0 };
	int err;

	err = times_from_user_timespec(&attr, times);
	if (err || !attr.valid)
		return err;

	return setattr_fd(fd, &attr);
}

u64 sys_chmod(void)
{
	const char *path = syscall_arg_ptr(0);
	umode_t mode = syscall_arg_raw(1);
	struct fs_iattr attr = {
		.valid = ATTR_MODE,
		.mode = mode & 07777,
	};

	return setattr_path_at(AT_FDCWD, path, &attr, LOOKUP_FOLLOW);
}

u64 sys_fchmod(void)
{
	int fd = syscall_arg_int(0);
	umode_t mode = syscall_arg_raw(1);
	struct fs_iattr attr = {
		.valid = ATTR_MODE,
		.mode = mode & 07777,
	};

	return setattr_fd(fd, &attr);
}

u64 sys_fchmodat(void)
{
	int dirfd = syscall_arg_int(0);
	const char *path = syscall_arg_ptr(1);
	umode_t mode = syscall_arg_raw(2);
	struct fs_iattr attr = {
		.valid = ATTR_MODE,
		.mode = mode & 07777,
	};

	return setattr_path_at(dirfd, path, &attr, LOOKUP_FOLLOW);
}

static void chown_attr_init(struct fs_iattr *attr, long uid, long gid)
{
	memset(attr, 0, sizeof(*attr));

	if (uid != -1) {
		attr->valid |= ATTR_UID;
		attr->uid = (kuid_t)uid;
	}
	if (gid != -1) {
		attr->valid |= ATTR_GID;
		attr->gid = (kgid_t)gid;
	}
}

u64 sys_chown(void)
{
	const char *path = syscall_arg_ptr(0);
	long uid = syscall_arg_int(1);
	long gid = syscall_arg_int(2);
	struct fs_iattr attr;

	chown_attr_init(&attr, uid, gid);
	if (!attr.valid)
		return 0;

	return setattr_path_at(AT_FDCWD, path, &attr, LOOKUP_FOLLOW);
}

u64 sys_fchown(void)
{
	int fd = syscall_arg_int(0);
	long uid = syscall_arg_int(1);
	long gid = syscall_arg_int(2);
	struct fs_iattr attr;

	chown_attr_init(&attr, uid, gid);
	if (!attr.valid)
		return 0;

	return setattr_fd(fd, &attr);
}

u64 sys_fchownat(void)
{
	int dirfd = syscall_arg_int(0);
	const char *path = syscall_arg_ptr(1);
	long uid = syscall_arg_int(2);
	long gid = syscall_arg_int(3);
	int flags = syscall_arg_raw(4);
	struct fs_iattr attr;
	unsigned int lookup_flags;

	chown_attr_init(&attr, uid, gid);
	if (!attr.valid)
		return 0;

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
		return -EINVAL;

	if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0')
		return setattr_fd(dirfd, &attr);

	lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	return setattr_path_at(dirfd, path, &attr, lookup_flags);
}

u64 sys_utime(void)
{
	const char *path = syscall_arg_ptr(0);
	const struct utimbuf *utimbuf = syscall_arg_ptr(1);
	struct timespec times[2];

	if (utimbuf) {
		struct utimbuf ub;

		if (copy_from_user(&ub, utimbuf, sizeof(ub)))
			return -EFAULT;
		times[0].tv_sec = ub.actime;
		times[0].tv_nsec = 0;
		times[1].tv_sec = ub.modtime;
		times[1].tv_nsec = 0;
		return set_times_path_at(AT_FDCWD, path, times,
					 LOOKUP_FOLLOW);
	}

	return set_times_path_at(AT_FDCWD, path, NULL, LOOKUP_FOLLOW);
}

static int timeval_pair_to_timespec(const struct timeval tv[2],
				    struct timespec ts[2])
{
	for (int i = 0; i < 2; i++) {
		if (tv[i].tv_usec < 0 || tv[i].tv_usec >= 1000000)
			return -EINVAL;
		ts[i].tv_sec = tv[i].tv_sec;
		ts[i].tv_nsec = (long)tv[i].tv_usec * 1000L;
	}
	return 0;
}

u64 sys_utimes(void)
{
	const char *path = syscall_arg_ptr(0);
	const struct timeval *utimes = syscall_arg_ptr(1);
	struct timeval tv[2];
	struct timespec ts[2];
	int err;

	if (!utimes)
		return set_times_path_at(AT_FDCWD, path, NULL, LOOKUP_FOLLOW);

	if (copy_from_user(tv, utimes, sizeof(tv)))
		return -EFAULT;
	err = timeval_pair_to_timespec(tv, ts);
	if (err)
		return err;

	return set_times_path_at(AT_FDCWD, path, ts, LOOKUP_FOLLOW);
}

u64 sys_futimesat(void)
{
	int dirfd = syscall_arg_int(0);
	const char *path = syscall_arg_ptr(1);
	const struct timeval *utimes = syscall_arg_ptr(2);
	struct timeval tv[2];
	struct timespec ts[2];
	int err;

	if (!path)
		return -EINVAL;

	if (!utimes)
		return set_times_path_at(dirfd, path, NULL, LOOKUP_FOLLOW);

	if (copy_from_user(tv, utimes, sizeof(tv)))
		return -EFAULT;
	err = timeval_pair_to_timespec(tv, ts);
	if (err)
		return err;

	return set_times_path_at(dirfd, path, ts, LOOKUP_FOLLOW);
}

u64 sys_utimensat(void)
{
	int dirfd = syscall_arg_int(0);
	const char *path = syscall_arg_ptr(1);
	const struct timespec *utimes = syscall_arg_ptr(2);
	int flags = syscall_arg_raw(3);
	struct timespec ts[2];

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
		return -EINVAL;

	if (utimes && copy_from_user(ts, utimes, sizeof(ts)))
		return -EFAULT;

	unsigned int lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ?
					    0 :
					    LOOKUP_FOLLOW;

	if (!path || ((flags & AT_EMPTY_PATH) && path[0] == '\0'))
		return set_times_fd(dirfd, utimes ? ts : NULL);

	return set_times_path_at(dirfd, path, utimes ? ts : NULL,
				 lookup_flags);
}

static u64 do_fsync(int datasync)
{
	struct fs_file *fp = NULL;
	int ret;
	int fd;

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	/* Filesystems with nothing to persist (pipes, tmpfs, ...) have no
	 * ->fsync; treat the request as a successful no-op. */
	if (!fp->ops || !fp->ops->fsync) {
		fs_file_put(fp);
		return 0;
	}

	ret = fp->ops->fsync(fp, 0, -1, datasync);
	fs_file_put(fp);
	return ret;
}

u64 sys_fsync(void)
{
	return do_fsync(0);
}

u64 sys_fdatasync(void)
{
	return do_fsync(1);
}

u64 sys_sync(void)
{
	sync_all_filesystems();
	return 0;
}

u64 sys_syncfs(void)
{
	struct fs_file *fp = NULL;
	int ret;
	int fd;

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!fp->inode) {
		fs_file_put(fp);
		return -EINVAL;
	}
	ret = sync_filesystem(fp->inode->sb);
	fs_file_put(fp);
	return ret;
}
