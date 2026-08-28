#include <brk/base/error.h>
#include <brk/base/kernel.h>
#include <brk/base/types.h>
#include <brk/fs/dcache.h>
#include <brk/fs/fdtable.h>
#include <brk/fs/fs.h>
#include <brk/fs/fs_types.h>
#include <brk/fs/fsinfo.h>
#include <brk/fs/mount.h>
#include <brk/fs/path.h>
#include <brk/lib/string.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <brk/printk/printk.h>
#include <brk/process/processor.h>
#include <brk/process/task.h>
#include <brk/syscall/syscall.h>
#include <uapi/brk/dirent.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/fcntl.h>
#include <uapi/brk/limits.h>
#include <uapi/brk/stat.h>
#include <uapi/brk/types.h>
#include <uapi/brk/utsname.h>

uint64_t sys_read(void)
{
	int fd;
	void *buf;
	size_t n;
	struct fs_file *fp;
	ssize_t ret;
	struct task_control_block *task;

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

uint64_t sys_write(void)
{
	int fd;
	const void *buf;
	size_t n;
	struct fs_file *fp;
	ssize_t ret;
	struct task_control_block *task;

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

uint64_t sys_open(void)
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

uint64_t sys_openat(void)
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

uint64_t sys_close(void)
{
	int fd = syscall_arg_int(0);
	struct task_control_block *task = current_task();
	return fdtable_close_fd(task->fdtable, fd);
}

uint64_t sys_execve(void)
{
	char *path = syscall_arg_ptr(0);
	char **argv = syscall_arg_ptr(1);
	char **envp = syscall_arg_ptr(2);
	return do_execve(path, argv, envp);
}

uint64_t sys_mmap(void)
{
	return -ENOSYS;
}

uint64_t sys_munmap(void)
{
	return -ENOSYS;
}

uint64_t sys_mprotect(void)
{
	return -ENOSYS;
}

uint64_t sys_msync(void)
{
	return -ENOSYS;
}

uint64_t sys_mremap(void)
{
	return -ENOSYS;
}

uint64_t sys_fstat(void)
{
	struct fs_file *fp = NULL;
	struct stat *buf;
	int ret;
	int fd;
	struct task_control_block *task = current_task();

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);
	buf = syscall_arg_ptr(1);
	ret = fs_file_stat(fp, buf);
	fs_file_put(fp);
	return ret;
}

uint64_t sys_lstat(void)
{
	return -ENOSYS;
}

uint64_t sys_stat(void)
{
	int ret;
	const char *path = syscall_arg_ptr(0);
	struct stat *buf = syscall_arg_ptr(1);
	struct fs_file *fp = NULL;

	fp = do_openat(AT_FDCWD, path, O_RDONLY, 0);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	ret = fs_file_stat(fp, buf);
	fs_file_put(fp);
	return ret;
}

uint64_t sys_link(void)
{
	char *oldpath = syscall_arg_ptr(0);
	char *newpath = syscall_arg_ptr(1);
	return do_linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

uint64_t sys_linkat(void)
{
	int olddirfd = syscall_arg_int(0);
	char *oldpath = syscall_arg_ptr(1);
	int newdirfd = syscall_arg_int(2);
	char *newpath = syscall_arg_ptr(3);
	int flags = syscall_arg_raw(4);
	return do_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}

uint64_t sys_unlink(void)
{
	char *path = syscall_arg_ptr(0);
	return do_unlinkat(AT_FDCWD, path, 0);
}

uint64_t sys_unlinkat(void)
{
	int dirfd = syscall_arg_int(0);
	char *path = syscall_arg_ptr(1);
	int flags = syscall_arg_int(2);
	return do_unlinkat(dirfd, path, flags);
}

uint64_t sys_symlink(void)
{
	const char *target = syscall_arg_ptr(0);
	char *linkpath = syscall_arg_ptr(1);
	return do_symlinkat(AT_FDCWD, linkpath, target);
}

uint64_t sys_readlink(void)
{
	const char *pathname = syscall_arg_ptr(0);
	char *buf = syscall_arg_ptr(1);
	size_t bufsiz = syscall_arg_raw(2);
	return do_readlinkat(AT_FDCWD, pathname, buf, bufsiz);
}

uint64_t sys_rename(void)
{
	const char *oldpathname = syscall_arg_ptr(0);
	const char *newpathname = syscall_arg_ptr(1);
	return do_renameat(AT_FDCWD, oldpathname, AT_FDCWD, newpathname, 0);
}

uint64_t sys_creat(void)
{
	const char *pathname = syscall_arg_ptr(0);
	umode_t mode = syscall_arg_raw(1);
	return do_creat(pathname, mode);
}

uint64_t sys_rmdir(void)
{
	const char *pathname = syscall_arg_ptr(0);
	return do_rmdir(pathname);
}

uint64_t sys_uname(void)
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

	strlcpy(name.machine, arch_uname_machine(), sizeof(name.machine));
	*buf = name;
	return 0;
}

uint64_t sys_getcwd(void)
{
	struct task_control_block *task = current_task();
	char *buf = syscall_arg_ptr(0);
	size_t size = syscall_arg_raw(1);
	struct fs_path cwd = { 0 };
	fsinfo_get_cwd(task->fsinfo, &cwd);
	int ret = fs_path_to_absolute(&cwd, buf, size);
	fs_path_put(&cwd);
	return ret;
}

uint64_t sys_chdir(void)
{
	char *pathname = syscall_arg_ptr(0);
	struct task_control_block *task = current_task();
	struct fs_path new_path;

	int err = fs_path_lookup(pathname, 0, &new_path);
	if (err)
		return err;

	if (!new_path.dentry->inode) {
		fs_path_put(&new_path);
		return -ENOENT;
	}

	if (!S_ISDIR(new_path.dentry->inode->mode)) {
		fs_path_put(&new_path);
		return -ENOTDIR;
	}

	fsinfo_update_cwd(task->fsinfo, &new_path);
	return 0;
}

uint64_t sys_fchdir(void)
{
	int fd;
	struct fs_file *fp;
	struct task_control_block *task = current_task();

	fd = syscall_arg_int(0);
	fp = fdtable_get_file(task->fdtable, fd);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	if (!(fp->mode & FMODE_DIR)) {
		fs_file_put(fp);
		return -ENOTDIR;
	}

	fs_path_get(&fp->path);
	fsinfo_update_cwd(task->fsinfo, &fp->path);
	fs_file_put(fp);
	return 0;
}

uint64_t sys_renameat(void)
{
	int olddirfd = syscall_arg_int(0);
	const char *oldpathname = syscall_arg_ptr(1);
	int newdirfd = syscall_arg_int(2);
	const char *newpathname = syscall_arg_ptr(3);
	return do_renameat(olddirfd, oldpathname, newdirfd, newpathname, 0);
}

uint64_t sys_renameat2(void)
{
	int olddirfd = syscall_arg_int(0);
	const char *oldpathname = syscall_arg_ptr(1);
	int newdirfd = syscall_arg_int(2);
	const char *newpathname = syscall_arg_ptr(3);
	int flags = syscall_arg_raw(4);
	return do_renameat(olddirfd, oldpathname, newdirfd, newpathname, flags);
}

uint64_t sys_symlinkat(void)
{
	const char *target;
	int newdirfd;
	const char *linkpath;

	target = syscall_arg_ptr(0);
	newdirfd = syscall_arg_int(1);
	linkpath = syscall_arg_ptr(2);
	return do_symlinkat(newdirfd, linkpath, target);
}

uint64_t sys_readlinkat(void)
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

uint64_t sys_mkdir(void)
{
	char *path = syscall_arg_ptr(0);
	return do_mkdirat(AT_FDCWD, path, 0);
}

uint64_t sys_mkdirat(void)
{
	int dirfd = syscall_arg_int(0);
	char *pathname = syscall_arg_ptr(1);
	return do_mkdirat(dirfd, pathname, 0);
}

uint64_t sys_mknod(void)
{
	char *path = syscall_arg_ptr(0);
	umode_t mode = syscall_arg_raw(1);
	dev_t dev = syscall_arg_raw(2);
	return do_mknodat(AT_FDCWD, path, mode, dev);
}

uint64_t sys_mknodat(void)
{
	int dirfd = syscall_arg_int(0);
	char *path = syscall_arg_ptr(1);
	umode_t mode = syscall_arg_raw(2);
	dev_t dev = syscall_arg_raw(3);
	return do_mknodat(dirfd, path, mode, dev);
}

uint64_t sys_pipe(void)
{
	int *pipefd = syscall_arg_ptr(0);
	int err;

	if (!pipefd)
		return -EFAULT;

	err = do_pipe2(pipefd, 0);
	return err < 0 ? (uint64_t)(long)err : 0;
}

uint64_t sys_pipe2(void)
{
	int *pipefd = syscall_arg_ptr(0);
	int flags = syscall_arg_int(1);

	if (!pipefd)
		return -EFAULT;

	return (uint64_t)(long)do_pipe2(pipefd, flags);
}

uint64_t sys_dup(void)
{
	int oldfd = syscall_arg_int(0);
	struct task_control_block *task = current_task();
	return fdtable_dup_fd(task->fdtable, oldfd);
}

uint64_t sys_dup2(void)
{
	int oldfd = syscall_arg_int(0);
	int newfd = syscall_arg_int(1);
	struct task_control_block *task = current_task();
	int err = fdtable_dup_fd2(task->fdtable, oldfd, newfd);
	if (err)
		return err;
	return newfd;
}

uint64_t sys_mount(void)
{
	const char *source = syscall_arg_ptr(0);
	const char *target = syscall_arg_ptr(1);
	const char *type = syscall_arg_ptr(2);
	unsigned long flags = syscall_arg_raw(3);
	void *data = syscall_arg_ptr(4);
	return do_mount(source, target, type, flags, data);
}

uint64_t sys_umount2(void)
{
	const char *target = syscall_arg_ptr(0);
	int flags = syscall_arg_int(1);
	struct fs_path path;
	int err;

	err = fs_path_lookup(target, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->lock);
	if (path.dentry->flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->lock);
		fs_path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->lock);

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

	fs_path_put(&path);
	return 0;
}

uint64_t sys_lseek(void)
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
	uint8_t *buf;
	size_t size;
	size_t pos;
};

static bool getdents_filldir(struct fs_dir_iterator *ctx, const char *name,
			     int namelen, loff_t offset, uint64_t ino,
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

uint64_t sys_getdents(void)
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

	struct fs_inode *inode = fp->path.dentry->inode;
	const struct fs_file_ops *fop = fp->ops;

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
	uint8_t *buf;
	size_t size;
	size_t pos;
};

static bool getdents64_filldir(struct fs_dir_iterator *ctx, const char *name,
			       int namelen, loff_t offset, uint64_t ino,
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

uint64_t sys_getdents64(void)
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
		klog_info("%s(): Not a directory\n", __func__);
		fs_file_put(fp);
		return -ENOTDIR;
	}

	buf = syscall_arg_ptr(1);
	size = syscall_arg_raw(2);

	struct fs_inode *inode = fp->path.dentry->inode;
	const struct fs_file_ops *fop = fp->ops;

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

uint64_t sys_ioctl(void)
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
	return (uint64_t)(long)ret;
}

static uint64_t do_fsync(int datasync)
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

uint64_t sys_fsync(void)
{
	return do_fsync(0);
}

uint64_t sys_fdatasync(void)
{
	return do_fsync(1);
}

uint64_t sys_sync(void)
{
	sync_all_filesystems();
	return 0;
}

uint64_t sys_syncfs(void)
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
