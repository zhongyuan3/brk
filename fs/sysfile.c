#include <arch/asm/page.h>
#include <brk/align.h>
#include <brk/clocksource.h>
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
#include <brk/mutex.h>
#include <brk/path.h>
#include <brk/permission.h>
#include <brk/printk.h>
#include <brk/rwsem.h>
#include <brk/spinlock.h>
#include <brk/stat.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <stdalign.h>
#include <uapi/dirent.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>
#include <uapi/statvfs.h>
#include <uapi/sysinfo.h>
#include <uapi/time.h>
#include <uapi/uio.h>

long sys_read(const struct syscall_ctx *ctx)
{
	int fd;
	void *buf;
	size_t n;
	struct file *fp;
	ssize_t ret;
	struct thread *task;

	task = current_thread();
	fd = syscall_arg_int(ctx, 0);
	fp = fdtable_get_file(&task->process->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	buf = syscall_arg_ptr(ctx, 1);
	n = syscall_arg_raw(ctx, 2);
	ret = file_read(fp, buf, n);
	file_put(fp);
	return ret;
}

long sys_write(const struct syscall_ctx *ctx)
{
	int fd;
	const void *buf;
	size_t n;
	struct file *fp;
	ssize_t ret;
	struct thread *task;

	task = current_thread();
	fd = syscall_arg_int(ctx, 0);
	fp = fdtable_get_file(&task->process->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	if (fp->mode & FMODE_DIR) {
		file_put(fp);
		return -EISDIR;
	}
	buf = syscall_arg_ptr(ctx, 1);
	n = syscall_arg_raw(ctx, 2);
	ret = file_write(fp, buf, n);
	file_put(fp);
	return ret;
}

static u64 readv_writev(const struct syscall_ctx *ctx, bool is_write)
{
	int fd = syscall_arg_int(ctx, 0);
	const struct __brk_iovec *uiov = syscall_arg_ptr(ctx, 1);
	int iovcnt = syscall_arg_int(ctx, 2);
	struct __brk_iovec *iov;
	struct file *fp;
	size_t bytes;
	ssize_t total = 0;

	if (iovcnt < 0 || iovcnt > IOV_MAX)
		return -EINVAL;
	if (!iovcnt)
		return 0;
	if (!uiov)
		return -EFAULT;

	bytes = (size_t)iovcnt * sizeof(*iov);
	iov = kmalloc(bytes);
	if (!iov)
		return -ENOMEM;
	if (copy_from_user(iov, uiov, bytes)) {
		kfree(iov);
		return -EFAULT;
	}

	fp = fdtable_get_file(&current_process()->fdtable, fd);
	if (IS_ERR(fp)) {
		kfree(iov);
		return PTR_ERR(fp);
	}
	if (is_write && (fp->mode & FMODE_DIR)) {
		file_put(fp);
		kfree(iov);
		return -EISDIR;
	}

	for (int i = 0; i < iovcnt; i++) {
		ssize_t n;

		if (!iov[i].iov_len)
			continue;
		if (!user_range_ok((user_addr_t)(uintptr_t)iov[i].iov_base,
				   iov[i].iov_len)) {
			if (!total)
				total = -EFAULT;
			break;
		}

		if (is_write)
			n = file_write(fp, iov[i].iov_base, iov[i].iov_len);
		else
			n = file_read(fp, iov[i].iov_base, iov[i].iov_len);

		if (n < 0) {
			if (!total)
				total = n;
			break;
		}
		total += n;
		if ((size_t)n != iov[i].iov_len)
			break;
	}

	file_put(fp);
	kfree(iov);
	return (u64)(long)total;
}

long sys_read_iov(const struct syscall_ctx *ctx)
{
	return readv_writev(ctx, false);
}

long sys_write_iov(const struct syscall_ctx *ctx)
{
	return readv_writev(ctx, true);
}

long sys_open(const struct syscall_ctx *ctx)
{
	int fd;
	struct file *fp = NULL;
	int dirfd = syscall_arg_raw(ctx, 0);
	char *path = syscall_arg_ptr(ctx, 1);
	int flags = syscall_arg_raw(ctx, 2);
	umode_t mode = syscall_arg_raw(ctx, 3);

	fp = do_openat(dirfd, path, flags, mode);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	fd = fdtable_alloc_fd(&current_process()->fdtable, fp);
	if (fd < 0) {
		file_put(fp);
		return -EMFILE;
	}
	return fd;
}

long sys_close(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	struct thread *task = current_thread();
	return fdtable_close_fd(&task->process->fdtable, fd);
}

long sys_exec(const struct syscall_ctx *ctx)
{
	char *path = syscall_arg_ptr(ctx, 0);
	char **argv = syscall_arg_ptr(ctx, 1);
	char **envp = syscall_arg_ptr(ctx, 2);
	return do_execve(path, argv, envp);
}

static u64 stat_path_at(int dirfd, const char *path, struct kstat *buf,
			unsigned int flags)
{
	struct path p;
	struct inode *inode;
	int ret;

	ret = path_lookup_at(dirfd, path, flags, &p);
	if (ret)
		return ret;

	inode = p.dentry->inode;
	if (!inode) {
		path_put(&p);
		return -ENOENT;
	}
	if (!inode->ops || !inode->ops->getattr) {
		path_put(&p);
		return -EOPNOTSUPP;
	}

	ret = inode->ops->getattr(inode, buf, 0, 0);
	path_put(&p);
	return ret;
}

static u64 stat_fd(struct thread *task, int fd, struct kstat *ks)
{
	struct file *fp;
	int ret;

	fp = fdtable_get_file(&task->process->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	ret = file_stat(fp, ks);
	file_put(fp);
	return ret;
}

long sys_stat(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	const char *path = syscall_arg_ptr(ctx, 1);
	int flags = syscall_arg_int(ctx, 2);
	struct __brk_stat *ubuf = syscall_arg_ptr(ctx, 3);
	struct __brk_stat stx;
	struct kstat ks;
	int ret;

	if (!ubuf)
		return -EFAULT;
	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
		return -EINVAL;

	if ((flags & AT_EMPTY_PATH) && (!path || path[0] == '\0')) {
		ret = stat_fd(current_thread(), dirfd, &ks);
	} else {
		unsigned int lookup_flags =
			(flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;

		ret = stat_path_at(dirfd, path, &ks, lookup_flags);
	}
	if (ret)
		return ret;

	kstat_to_statx(&ks, &stx);
	if (copy_to_user(ubuf, &stx, sizeof(stx)))
		return -EFAULT;
	return 0;
}

static u64 access_path_at(int dirfd, const char *path, int mode,
			  unsigned int lookup_flags)
{
	struct path p;
	struct inode *inode;
	int ret;

	if (mode & ~7)
		return -EINVAL;

	ret = path_lookup_at(dirfd, path, lookup_flags, &p);
	if (ret)
		return ret;

	inode = p.dentry->inode;
	if (!inode) {
		path_put(&p);
		return -ENOENT;
	}

	ret = inode_permission(inode, mode & 7);
	path_put(&p);
	return ret;
}

long sys_access(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	const char *path = syscall_arg_ptr(ctx, 1);
	int mode = syscall_arg_int(ctx, 2);
	int flags = syscall_arg_int(ctx, 3);
	unsigned int lookup_flags;

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS))
		return -EINVAL;

	lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	return access_path_at(dirfd, path, mode, lookup_flags);
}

long sys_stat_fs_fd(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	struct __brk_statfs *ubuf = syscall_arg_ptr(ctx, 1);
	struct __brk_statfs st;
	struct kstatfs ks;
	struct thread *task = current_thread();
	struct file *fp;
	int ret;

	fp = fdtable_get_file(&task->process->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	if (!ubuf) {
		file_put(fp);
		return -EFAULT;
	}

	ret = statfs(fp->inode->sb, &ks);
	file_put(fp);
	if (ret)
		return ret;

	kstatfs_to_statfs(&ks, &st);
	if (copy_to_user(ubuf, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

long sys_stat_fs(const struct syscall_ctx *ctx)
{
	const char *path = syscall_arg_ptr(ctx, 0);
	struct __brk_statfs *ubuf = syscall_arg_ptr(ctx, 1);
	struct __brk_statfs st;
	struct kstatfs ks;
	struct path p = { 0 };
	struct inode *inode;
	int ret;

	if (!ubuf)
		return -EFAULT;

	ret = path_lookup(path, LOOKUP_FOLLOW, &p);
	if (ret)
		return ret;

	inode = p.dentry ? p.dentry->inode : NULL;
	if (!inode) {
		path_put(&p);
		return -ENOENT;
	}

	ret = statfs(inode->sb, &ks);
	path_put(&p);
	if (ret)
		return ret;

	kstatfs_to_statfs(&ks, &st);
	if (copy_to_user(ubuf, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

long sys_preallocate(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	int mode = syscall_arg_int(ctx, 1);
	loff_t offset = (loff_t)syscall_arg_raw(ctx, 2);
	loff_t len = (loff_t)syscall_arg_raw(ctx, 3);
	struct thread *task = current_thread();
	struct file *fp;
	int ret;

	if (offset < 0 || len <= 0)
		return -EINVAL;

	fp = fdtable_get_file(&task->process->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_WRITE)) {
		file_put(fp);
		return -EINVAL;
	}

	ret = file_fallocate(fp, mode, offset, len);
	file_put(fp);
	return ret;
}

long sys_link(const struct syscall_ctx *ctx)
{
	int olddirfd = syscall_arg_int(ctx, 0);
	char *oldpath = syscall_arg_ptr(ctx, 1);
	int newdirfd = syscall_arg_int(ctx, 2);
	char *newpath = syscall_arg_ptr(ctx, 3);
	int flags = syscall_arg_raw(ctx, 4);
	return do_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}

long sys_unlink(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	char *path = syscall_arg_ptr(ctx, 1);
	int flags = syscall_arg_int(ctx, 2);
	return do_unlinkat(dirfd, path, flags);
}

long sys_get_sysinfo(const struct syscall_ctx *ctx)
{
	struct __brk_sysinfo *ubuf = syscall_arg_ptr(ctx, 0);
	struct __brk_sysinfo info = { 0 };

	if (!ubuf)
		return -EFAULT;

	strlcpy(info.sysname, "BRK", sizeof(info.sysname));
	strlcpy(info.release, BRK_VERSION, sizeof(info.release));
	strlcpy(info.machine, uname_machine(), sizeof(info.machine));
	info.page_size = PAGE_SIZE;
	info.hz = HZ;
	info.mono_resolution_ns = clocksource_get_resolution_ns();
	info.cpu_resolution_ns = TICK_NS;

	if (copy_to_user(ubuf, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

long sys_get_cwd(const struct syscall_ctx *ctx)
{
	struct thread *task = current_thread();
	char *buf = syscall_arg_ptr(ctx, 0);
	size_t size = syscall_arg_raw(ctx, 1);
	struct path cwd = { 0 };
	fsinfo_get_cwd(&task->process->fsinfo, &cwd);
	int ret = path_to_absolute(&cwd, buf, size);
	path_put(&cwd);
	return ret;
}

long sys_chdir(const struct syscall_ctx *ctx)
{
	char *pathname = syscall_arg_ptr(ctx, 0);
	struct thread *task = current_thread();
	struct path new_path;

	int err = path_lookup(pathname, LOOKUP_FOLLOW, &new_path);
	if (err)
		return err;

	struct inode *inode = dentry_inode(new_path.dentry);

	if (!inode) {
		path_put(&new_path);
		return -ENOENT;
	}

	if (!S_ISDIR(inode->mode)) {
		path_put(&new_path);
		return -ENOTDIR;
	}

	int perm = inode_permission(inode, MAY_EXEC);
	if (perm) {
		path_put(&new_path);
		return perm;
	}

	fsinfo_update_cwd(&task->process->fsinfo, &new_path);
	return 0;
}

long sys_chdir_fd(const struct syscall_ctx *ctx)
{
	int fd;
	struct file *fp;
	struct thread *task = current_thread();

	fd = syscall_arg_int(ctx, 0);
	fp = fdtable_get_file(&task->process->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_DIR)) {
		file_put(fp);
		return -ENOTDIR;
	}

	int perm = inode_permission(fp->inode, MAY_EXEC);
	if (perm) {
		file_put(fp);
		return perm;
	}

	path_get(&fp->path);
	fsinfo_update_cwd(&task->process->fsinfo, &fp->path);
	file_put(fp);
	return 0;
}

long sys_rename(const struct syscall_ctx *ctx)
{
	int olddirfd = syscall_arg_int(ctx, 0);
	const char *oldpathname = syscall_arg_ptr(ctx, 1);
	int newdirfd = syscall_arg_int(ctx, 2);
	const char *newpathname = syscall_arg_ptr(ctx, 3);
	return do_renameat(olddirfd, oldpathname, newdirfd, newpathname, 0);
}

long sys_symlink(const struct syscall_ctx *ctx)
{
	const char *target;
	int newdirfd;
	const char *linkpath;

	target = syscall_arg_ptr(ctx, 0);
	newdirfd = syscall_arg_int(ctx, 1);
	linkpath = syscall_arg_ptr(ctx, 2);
	return do_symlinkat(newdirfd, linkpath, target);
}

long sys_readlink(const struct syscall_ctx *ctx)
{
	int dirfd;
	const char *pathname;
	char *buf;
	size_t bufsiz;

	dirfd = syscall_arg_int(ctx, 0);
	pathname = syscall_arg_ptr(ctx, 1);
	buf = syscall_arg_ptr(ctx, 2);
	bufsiz = syscall_arg_raw(ctx, 3);
	return do_readlinkat(dirfd, pathname, buf, bufsiz);
}

long sys_mkdir(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	char *pathname = syscall_arg_ptr(ctx, 1);
	umode_t mode = syscall_arg_raw(ctx, 2);

	return do_mkdirat(dirfd, pathname, mode);
}

long sys_mknod(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	char *path = syscall_arg_ptr(ctx, 1);
	umode_t mode = syscall_arg_raw(ctx, 2);
	dev_t dev = syscall_arg_raw(ctx, 3);
	return do_mknodat(dirfd, path, mode, dev);
}

long sys_pipe(const struct syscall_ctx *ctx)
{
	int *pipefd = syscall_arg_ptr(ctx, 0);
	int flags = syscall_arg_int(ctx, 1);

	if (!pipefd)
		return -EFAULT;

	return (u64)(long)do_pipe2(pipefd, flags);
}

long sys_dup(const struct syscall_ctx *ctx)
{
	int oldfd = syscall_arg_int(ctx, 0);
	struct thread *task = current_thread();
	return fdtable_dup_fd(&task->process->fdtable, oldfd);
}

long sys_dup_to(const struct syscall_ctx *ctx)
{
	int oldfd = syscall_arg_int(ctx, 0);
	int newfd = syscall_arg_int(ctx, 1);
	int flags = syscall_arg_int(ctx, 2);
	struct thread *task = current_thread();
	int err;

	if (oldfd == newfd)
		return -EINVAL;
	if (flags & ~O_CLOEXEC)
		return -EINVAL;

	err = fdtable_dup_fd2(&task->process->fdtable, oldfd, newfd);
	if (err)
		return err;
	return newfd;
}

long sys_seek(const struct syscall_ctx *ctx)
{
	int fd = 0;
	struct file *fp = NULL;
	int ret;
	off_t off;
	int whence;

	fd = syscall_arg_int(ctx, 0);
	fp = fdtable_get_file(&current_process()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	off = syscall_arg_raw(ctx, 1);
	whence = syscall_arg_int(ctx, 2);

	ret = file_lseek(fp, off, whence);
	file_put(fp);
	return ret;
}

struct getdents_context {
	struct dir_iterator ctx;
	u8 *buf;
	size_t size;
	size_t pos;
};

static bool getdents_filldir(struct dir_iterator *ctx, const char *name,
			     int namelen, loff_t offset, u64 ino,
			     unsigned int d_type)
{
	struct getdents_context *gctx;
	struct __brk_dirent *de;

	(void)offset;

	gctx = container_of(ctx, struct getdents_context, ctx);
	if (gctx->pos > gctx->size)
		return false;

	size_t len = offsetof(struct __brk_dirent, d_name) + namelen + 1;
	len = align_up(len, alignof(struct __brk_dirent));

	if (len > gctx->size - gctx->pos || len > 0xffffffffu)
		return false;

	de = (struct __brk_dirent *)(gctx->buf + gctx->pos);
	de->d_ino = ino;
	de->d_reclen = (u32)len;
	de->d_type = d_type;
	memcpy(de->d_name, name, namelen);
	de->d_name[namelen] = '\0';
	gctx->pos += len;

	return true;
}

long sys_read_dir(const struct syscall_ctx *ctx)
{
	int fd = -1;
	struct file *fp = NULL;
	void *buf;
	size_t size;
	int flags;
	int err;

	fd = syscall_arg_int(ctx, 0);
	buf = syscall_arg_ptr(ctx, 1);
	size = syscall_arg_raw(ctx, 2);
	flags = syscall_arg_int(ctx, 3);

	if (flags)
		return -EINVAL;

	fp = fdtable_get_file(&current_process()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_DIR)) {
		file_put(fp);
		return -ENOTDIR;
	}

	struct inode *inode = dentry_inode(fp->path.dentry);
	const struct file_ops *fop = fp->ops;

	if (!inode) {
		file_put(fp);
		return -ENOENT;
	}
	if (!fop->iterate_shared) {
		file_put(fp);
		return -EOPNOTSUPP;
	}

	mutex_lock(&fp->pos_lock);

	struct getdents_context dctx = {
		.ctx.actor = getdents_filldir,
		.ctx.pos = fp->pos,
		.buf = buf,
		.size = size,
		.pos = 0,
	};

	down_read(&inode->rwsem);
	err = fop->iterate_shared(fp, &dctx.ctx);
	up_read(&inode->rwsem);
	if (err) {
		mutex_unlock(&fp->pos_lock);
		file_put(fp);
		return err;
	}

	fp->pos = dctx.ctx.pos;
	mutex_unlock(&fp->pos_lock);
	file_put(fp);

	return dctx.pos;
}

long sys_ioctl(const struct syscall_ctx *ctx)
{
	struct file *fp;
	int fd;

	fd = syscall_arg_int(ctx, 0);
	fp = fdtable_get_file(&current_process()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	unsigned int cmd = (unsigned int)syscall_arg_raw(ctx, 1);
	unsigned long arg = (unsigned long)syscall_arg_raw(ctx, 2);
	long ret = file_ioctl(fp, cmd, arg);
	file_put(fp);
	return (u64)(long)ret;
}

long sys_truncate_fd(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	loff_t length = (loff_t)syscall_arg_raw(ctx, 1);
	struct file *fp;
	int ret;

	if (length < 0)
		return -EINVAL;

	fp = fdtable_get_file(&current_process()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_WRITE)) {
		file_put(fp);
		return -EINVAL;
	}

	ret = file_truncate(fp, length);
	file_put(fp);
	return ret;
}

long sys_truncate(const struct syscall_ctx *ctx)
{
	const char *pathname = syscall_arg_ptr(ctx, 0);
	loff_t length = (loff_t)syscall_arg_raw(ctx, 1);
	struct iattr attr = {
		.valid = ATTR_SIZE,
		.size = length,
	};
	struct path path;
	struct inode *inode;
	int err;

	if (length < 0)
		return -EINVAL;

	err = path_lookup(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = path.dentry->inode;
	if (!inode) {
		path_put(&path);
		return -ENOENT;
	}
	if (!S_ISREG(inode->mode)) {
		path_put(&path);
		return S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
	}
	if (!inode->ops || !inode->ops->setattr) {
		path_put(&path);
		return -EOPNOTSUPP;
	}

	int perm = inode_permission(inode, MAY_WRITE);
	if (perm) {
		path_put(&path);
		return perm;
	}

	remove_inode_privs(inode, path.dentry);

	err = inode->ops->setattr(path.dentry, &attr);
	path_put(&path);
	return err;
}

static int check_setattr_perm(struct inode *inode, struct iattr *attr)
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

static int setattr_path_at(int dirfd, const char *path, struct iattr *attr,
			   unsigned int lookup_flags)
{
	struct path p;
	struct inode *inode;
	int err;

	if (!path)
		return -EINVAL;

	err = path_lookup_at(dirfd, path, lookup_flags, &p);
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
	path_put(&p);
	return err;
}

static int setattr_fd(int fd, struct iattr *attr)
{
	struct file *fp;
	int err;

	fp = fdtable_get_file(&current_process()->fdtable, fd);
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
	file_put(fp);
	return err;
}

static int times_from_user_timespec(struct iattr *attr,
				    const struct __brk_timespec times[2])
{
	if (!times) {
		ktime_t now = ktime_realtime();

		attr->valid |= ATTR_ATIME | ATTR_MTIME;
		attr->atime = now;
		attr->mtime = now;
		return 0;
	}

	for (int i = 0; i < 2; i++) {
		const struct __brk_timespec *ts = &times[i];
		ktime_t t;

		if (ts->tv_nsec == UTIME_OMIT)
			continue;
		if (ts->tv_nsec == UTIME_NOW) {
			t = ktime_realtime();
		} else {
			if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
				return -EINVAL;
			t = ktime_from_timespec(ts);
		}

		if (i == 0) {
			attr->atime = t;
			attr->valid |= ATTR_ATIME;
		} else {
			attr->mtime = t;
			attr->valid |= ATTR_MTIME;
		}
	}
	return 0;
}

static int set_times_path_at(int dirfd, const char *path,
			     const struct __brk_timespec times[2],
			     unsigned int lookup_flags)
{
	struct iattr attr = { 0 };
	int err;

	err = times_from_user_timespec(&attr, times);
	if (err || !attr.valid)
		return err;

	return setattr_path_at(dirfd, path, &attr, lookup_flags);
}

static int set_times_fd(int fd, const struct __brk_timespec times[2])
{
	struct iattr attr = { 0 };
	int err;

	err = times_from_user_timespec(&attr, times);
	if (err || !attr.valid)
		return err;

	return setattr_fd(fd, &attr);
}

long sys_chmod(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	const char *path = syscall_arg_ptr(ctx, 1);
	umode_t mode = syscall_arg_raw(ctx, 2);
	int flags = syscall_arg_int(ctx, 3);
	unsigned int lookup_flags;
	struct iattr attr = {
		.valid = ATTR_MODE,
		.mode = mode & 07777,
	};

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
		return -EINVAL;
	if ((flags & AT_EMPTY_PATH) && (!path || path[0] == '\0'))
		return setattr_fd(dirfd, &attr);

	lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	return setattr_path_at(dirfd, path, &attr, lookup_flags);
}

static void chown_attr_init(struct iattr *attr, long uid, long gid)
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

long sys_chown(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	const char *path = syscall_arg_ptr(ctx, 1);
	long uid = syscall_arg_int(ctx, 2);
	long gid = syscall_arg_int(ctx, 3);
	int flags = syscall_arg_raw(ctx, 4);
	struct iattr attr;
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

long sys_set_file_times(const struct syscall_ctx *ctx)
{
	int dirfd = syscall_arg_int(ctx, 0);
	const char *path = syscall_arg_ptr(ctx, 1);
	const struct __brk_timespec *utimes = syscall_arg_ptr(ctx, 2);
	int flags = syscall_arg_raw(ctx, 3);
	struct __brk_timespec ts[2];

	if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
		return -EINVAL;

	if (utimes && copy_from_user(ts, utimes, sizeof(ts)))
		return -EFAULT;

	unsigned int lookup_flags =
		(flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;

	if (!path || ((flags & AT_EMPTY_PATH) && path[0] == '\0'))
		return set_times_fd(dirfd, utimes ? ts : NULL);

	return set_times_path_at(dirfd, path, utimes ? ts : NULL, lookup_flags);
}

static u64 do_fsync(const struct syscall_ctx *ctx, int datasync)
{
	struct file *fp = NULL;
	int ret;
	int fd;

	fd = syscall_arg_int(ctx, 0);
	fp = fdtable_get_file(&current_process()->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	/* Filesystems with nothing to persist (pipes, tmpfs, ...) have no
	 * ->fsync; treat the request as a successful no-op. */
	if (!fp->ops || !fp->ops->fsync) {
		file_put(fp);
		return 0;
	}

	ret = fp->ops->fsync(fp, 0, -1, datasync);
	file_put(fp);
	return ret;
}

long sys_sync_fd(const struct syscall_ctx *ctx)
{
	return do_fsync(ctx, 0);
}

long sys_sync_data_fd(const struct syscall_ctx *ctx)
{
	return do_fsync(ctx, 1);
}

long sys_sync(const struct syscall_ctx *ctx)
{
	(void)ctx;
	sync_all_filesystems();
	return 0;
}
