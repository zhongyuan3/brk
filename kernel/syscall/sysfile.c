#include <brk/dcache.h>
#include <brk/dirent.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fcntl.h>
#include <brk/fs.h>
#include <brk/fs_types.h>
#include <brk/kernel.h>
#include <brk/limits.h>
#include <brk/lock.h>
#include <brk/mount.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/types.h>
#include <brk/utsname.h>

u64 sys_read(void)
{
	int fd;
	void *buf;
	usize_t n;
	struct file *fp;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;
	if (fp->f_mode & FMODE_DIR)
		return -EISDIR;
	buf = syscall_arg_ptr(1);
	n = syscall_arg_raw(2);
	return file_read(fp, buf, n);
}

u64 sys_write(void)
{
	int fd;
	const void *buf;
	usize_t n;
	struct file *fp;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;
	if (fp->f_mode & FMODE_DIR)
		return -EISDIR;
	buf = syscall_arg_ptr(1);
	n = syscall_arg_raw(2);
	return file_write(fp, buf, n);
}

u64 sys_open(void)
{
	int fd;
	struct file *fp = NULL;
	char *path = syscall_arg_ptr(0);
	int flags = syscall_arg_raw(1);
	umode_t mode = syscall_arg_raw(2);

	fp = do_openat(AT_FDCWD, path, flags, mode);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	fd = proc_alloc_fd(current_process(), fp);
	if (fd < 0) {
		file_put(fp);
		return -EMFILE;
	}
	return fd;
}

u64 sys_openat(void)
{
	int fd;
	struct file *fp = NULL;
	int dirfd = syscall_arg_raw(0);
	char *path = syscall_arg_ptr(1);
	int flags = syscall_arg_raw(2);
	umode_t mode = syscall_arg_raw(3);

	fp = do_openat(dirfd, path, flags, mode);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	fd = proc_alloc_fd(current_process(), fp);
	if (fd < 0) {
		file_put(fp);
		return -EMFILE;
	}
	return fd;
}

u64 sys_close(void)
{
	int err;
	struct file *fp = NULL;
	int fd = 0;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;

	current_process()->ofiles[fd] = NULL;
	file_put(fp);
	return 0;
}

u64 sys_execve(void)
{
	char *path = syscall_arg_ptr(0);
	char **argv = syscall_arg_ptr(1);
	char **envp = syscall_arg_ptr(2);
	return do_execve(path, argv, envp);
}

u64 sys_mmap(void)
{
	return -ENOSYS;
}

u64 sys_munmap(void)
{
	return -ENOSYS;
}

u64 sys_mprotect(void)
{
	return -ENOSYS;
}

u64 sys_msync(void)
{
	return -ENOSYS;
}

u64 sys_mremap(void)
{
	return -ENOSYS;
}

u64 sys_fstat(void)
{
	struct file *fp = NULL;
	struct stat *buf;
	int err;

	err = syscall_arg_fd(0, NULL, &fp);
	if (err)
		return err;
	buf = syscall_arg_ptr(1);
	return file_stat(fp, buf);
}

u64 sys_lstat(void)
{
	struct file *fp = NULL;
	struct stat *buf;
	int err;

	err = syscall_arg_fd(0, NULL, &fp);
	if (err)
		return err;
	buf = syscall_arg_ptr(1);
	return file_stat(fp, buf);
}

u64 sys_stat(void)
{
	int err;
	const char *path = syscall_arg_ptr(0);
	struct stat *buf = syscall_arg_ptr(1);
	struct file *fp = NULL;

	fp = do_openat(AT_FDCWD, path, O_RDONLY, 0);
	if (IS_ERR(fp))
		return PTR_ERR(fp);

	err = file_stat(fp, buf);
	file_put(fp);
	return err;
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
	usize_t bufsiz = syscall_arg_raw(2);
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
		.release = "0.0.1",
		.version = "0.0.1",
		.machine = "riscv64",
		.domainname = "none",
	};
	struct utsname *buf = syscall_arg_ptr(0);
	*buf = name;
	return 0;
}

u64 sys_getcwd(void)
{
	struct process *proc = current_process();
	char *buf = syscall_arg_ptr(0);
	usize_t size = syscall_arg_raw(1);
	int err = path_to_absolute(&proc->cwd, buf, size);
	if (err) {
		klog_warn("%s(): %s\n", __func__, strerror(err));
		return err;
	}
	return 0;
}

u64 sys_chdir(void)
{
	char *pathname = syscall_arg_ptr(0);
	struct process *proc = current_process();
	struct path new_path;

	int err = path_lookup(pathname, 0, &new_path);
	if (err)
		return err;

	if (!new_path.dentry->d_inode) {
		path_put(&new_path);
		return -ENOENT;
	}

	if (!S_ISDIR(new_path.dentry->d_inode->i_mode)) {
		path_put(&new_path);
		return -ENOTDIR;
	}

	path_put(&proc->cwd);
	proc->cwd = new_path;
	return 0;
}

u64 sys_fchdir(void)
{
	int err;
	int fd = 0;
	struct file *fp = NULL;
	struct process *proc = current_process();

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;

	if (!(fp->f_mode & FMODE_DIR))
		return -ENOTDIR;

	path_dup(&fp->f_path);
	path_put(&proc->cwd);
	proc->cwd = fp->f_path;
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
	int err;

	err = syscall_arg_fd(1, &newdirfd, NULL);
	if (err)
		return err;
	target = syscall_arg_ptr(0);
	linkpath = syscall_arg_ptr(2);
	return do_symlinkat(newdirfd, linkpath, target);
}

u64 sys_readlinkat(void)
{
	int dirfd;
	const char *pathname;
	char *buf;
	usize_t bufsiz;
	int err;

	err = syscall_arg_fd(0, &dirfd, NULL);
	if (err)
		return err;
	pathname = syscall_arg_ptr(1);
	buf = syscall_arg_ptr(2);
	bufsiz = syscall_arg_raw(3);
	return do_readlinkat(dirfd, pathname, buf, bufsiz);
}

u64 sys_mkdir(void)
{
	char *path = syscall_arg_ptr(0);
	return do_mkdirat(AT_FDCWD, path, 0);
}

u64 sys_mkdirat(void)
{
	int dirfd = syscall_arg_int(0);
	char *pathname = syscall_arg_ptr(1);
	return do_mkdirat(dirfd, pathname, 0);
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
	int oldfd;
	int newfd;
	int err;
	struct file *fp = NULL;

	err = syscall_arg_fd(0, &oldfd, &fp);
	if (err)
		return err;

	fp = file_dup(fp);
	newfd = proc_alloc_fd(current_process(), fp);
	if (newfd >= 0) {
		return newfd;
	} else {
		file_put(fp);
		return -EMFILE;
	}
}

u64 sys_dup2(void)
{
	int oldfd;
	int newfd;
	int err;
	struct process *proc = current_process();
	struct file *f = NULL;

	err = syscall_arg_fd(0, &oldfd, &f);
	if (err)
		return err;

	newfd = syscall_arg_int(1);
	if (!(0 <= newfd && newfd < OPEN_MAX))
		return -ERANGE;

	if (proc->ofiles[newfd] && proc->ofiles[newfd] == f)
		return newfd;

	if (proc->ofiles[newfd] && proc->ofiles[newfd] != f) {
		file_put(proc->ofiles[newfd]);
		proc->ofiles[newfd] = file_dup(f);
		return newfd;
	}

	proc->ofiles[newfd] = file_dup(f);
	return newfd;
}

u64 sys_mount(void)
{
	const char *source = syscall_arg_ptr(0);
	const char *target = syscall_arg_ptr(1);
	const char *type = syscall_arg_ptr(2);
	unsigned long flags = syscall_arg_raw(3);
	void *data = syscall_arg_ptr(4);
	return do_mount(source, target, type, flags, data);
}

u64 sys_umount2(void)
{
	const char *target = syscall_arg_ptr(0);
	int flags = syscall_arg_int(1);
	struct path path;
	int err;

	err = path_lookup(target, 0, &path);
	if (err)
		return err;

	spinlock_acquire(&path.dentry->d_lock);
	if (path.dentry->d_flags & DCACHE_NEGATIVE) {
		spinlock_release(&path.dentry->d_lock);
		path_put(&path);
		return -ENOENT;
	}
	spinlock_release(&path.dentry->d_lock);

	struct mount *mnt = lookup_mount(&path);
	if (!mnt) {
		path_put(&path);
		return -ENOENT;
	}

	err = do_umount(mnt, flags);
	if (err) {
		mount_put(mnt);
		path_put(&path);
		return err;
	}

	path_put(&path);
	return 0;
}

u64 sys_lseek(void)
{
	int fd = 0;
	struct file *fp = NULL;
	int err;
	off_t off;
	int whence;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;
	off = syscall_arg_raw(1);
	whence = syscall_arg_int(2);

	return file_lseek(fp, off, whence);
}

struct getdents_context {
	struct dir_context ctx;
	u8 *buf;
	usize_t size;
	usize_t pos;
};

static bool getdents_filldir(struct dir_context *ctx, const char *name,
			     int namelen, loff_t offset, u64 ino,
			     unsigned int d_type)
{
	struct getdents_context *gctx;
	struct dirent *de;

	gctx = container_of(ctx, struct getdents_context, ctx);
	if (gctx->pos > gctx->size)
		return false;

	usize_t len = offsetof(struct dirent, d_name) + namelen + 1;
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
	struct file *fp = NULL;
	void *buf;
	usize_t size;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;

	if (!(fp->f_mode & FMODE_DIR))
		return -ENOTDIR;

	buf = syscall_arg_ptr(1);
	size = syscall_arg_raw(2);

	struct inode *inode = fp->f_path.dentry->d_inode;
	const struct file_operations *fop = fp->f_op;

	if (!fop->iterate_shared)
		return -EOPNOTSUPP;

	sleeplock_acquire(&fp->f_pos_lock);

	struct getdents_context ctx = {
		.ctx.actor = getdents_filldir,
		.ctx.pos = fp->f_pos,
		.buf = buf,
		.size = size,
		.pos = 0,
	};

	sleeplock_acquire(&inode->i_rwsem);
	err = fop->iterate_shared(fp, &ctx.ctx);
	sleeplock_release(&inode->i_rwsem);
	if (err) {
		sleeplock_release(&fp->f_pos_lock);
		return err;
	}

	fp->f_pos = ctx.pos;
	sleeplock_release(&fp->f_pos_lock);

	return ctx.pos;
}

struct getdents64_context {
	struct dir_context ctx;
	u8 *buf;
	usize_t size;
	usize_t pos;
};

static bool getdents64_filldir(struct dir_context *ctx, const char *name,
			       int namelen, loff_t offset, u64 ino,
			       unsigned int d_type)
{
	struct getdents64_context *gctx;
	struct dirent64 *de;

	gctx = container_of(ctx, struct getdents64_context, ctx);
	if (gctx->pos > gctx->size)
		return false;

	usize_t len = offsetof(struct dirent64, d_name) + namelen + 1;
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
	struct file *fp = NULL;
	void *buf;
	usize_t size;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;

	if (!(fp->f_mode & FMODE_DIR)) {
		klog_info("%s(): Not a directory\n", __func__);
		return -ENOTDIR;
	}

	buf = syscall_arg_ptr(1);
	size = syscall_arg_raw(2);

	struct inode *inode = fp->f_path.dentry->d_inode;
	const struct file_operations *fop = fp->f_op;

	if (!fop->iterate_shared)
		return -EOPNOTSUPP;

	sleeplock_acquire(&fp->f_pos_lock);

	struct getdents64_context ctx = {
		.ctx.actor = getdents64_filldir,
		.ctx.pos = fp->f_pos,
		.buf = buf,
		.size = size,
		.pos = 0,
	};

	sleeplock_acquire(&inode->i_rwsem);
	err = fop->iterate_shared(fp, &ctx.ctx);
	sleeplock_release(&inode->i_rwsem);
	if (err) {
		sleeplock_release(&fp->f_pos_lock);
		return err;
	}

	fp->f_pos = ctx.pos;
	sleeplock_release(&fp->f_pos_lock);

	return ctx.pos;
}

u64 sys_ioctl(void)
{
	struct file *fp;
	int err;

	err = syscall_arg_fd(0, NULL, &fp);
	if (err)
		return (u64)(long)err;

	unsigned int cmd = (unsigned int)syscall_arg_raw(1);
	unsigned long arg = (unsigned long)syscall_arg_raw(2);
	long ret = file_ioctl(fp, cmd, arg);
	return (u64)(long)ret;
}
