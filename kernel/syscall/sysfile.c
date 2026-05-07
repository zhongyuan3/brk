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

static int do_pipe2(int *pipefd, int flags)
{
	struct file *rf, *wf;
	struct process *proc = current_process();
	int fd0, fd1, err;

	if (flags & ~(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;

	err = anon_pipe_create(&rf, &wf, (unsigned int)flags);
	if (err)
		return err;

	fd0 = proc_alloc_fd(proc, rf);
	if (fd0 < 0) {
		file_put(wf);
		file_put(rf);
		return -EMFILE;
	}

	fd1 = proc_alloc_fd(proc, wf);
	if (fd1 < 0) {
		proc->ofiles[fd0] = NULL;
		file_put(wf);
		file_put(rf);
		return -EMFILE;
	}

	pipefd[0] = fd0;
	pipefd[1] = fd1;
	return 0;
}

uint64_t sys_read(void)
{
	int fd;
	void *buf;
	size_t n;
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

uint64_t sys_write(void)
{
	int fd;
	const void *buf;
	size_t n;
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

uint64_t sys_open(void)
{
	int fd;
	struct file *fp = NULL;
	char *path = syscall_arg_ptr(0);
	int flags = syscall_arg_raw(1);
	mode_t mode = syscall_arg_raw(2);

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

uint64_t sys_openat(void)
{
	int fd;
	struct file *fp = NULL;
	int dirfd = syscall_arg_raw(0);
	char *path = syscall_arg_ptr(1);
	int flags = syscall_arg_raw(2);
	mode_t mode = syscall_arg_raw(3);

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

uint64_t sys_close(void)
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
	struct file *fp = NULL;
	struct stat *buf;
	int err;

	err = syscall_arg_fd(0, NULL, &fp);
	if (err)
		return err;
	buf = syscall_arg_ptr(1);
	return file_stat(fp, buf);
}

uint64_t sys_lstat(void)
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

uint64_t sys_stat(void)
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
	return -ENOSYS;
}

uint64_t sys_readlink(void)
{
	return -ENOSYS;
}

uint64_t sys_rename(void)
{
	return -ENOSYS;
}

uint64_t sys_creat(void)
{
	return -ENOSYS;
}

uint64_t sys_rmdir(void)
{
	return -ENOSYS;
}

uint64_t sys_uname(void)
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

uint64_t sys_getcwd(void)
{
	struct process *proc = current_process();
	char *buf = syscall_arg_ptr(0);
	size_t size = syscall_arg_raw(1);
	int err = path_to_absolute(&proc->cwd, buf, size);
	if (err) {
		log_warn("%s(): %s\n", __func__, strerror(err));
		return err;
	}
	return 0;
}

uint64_t sys_chdir(void)
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

uint64_t sys_fchdir(void)
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

uint64_t sys_renameat(void)
{
	return -ENOSYS;
}

uint64_t sys_symlinkat(void)
{
	return -ENOSYS;
}

uint64_t sys_readlinkat(void)
{
	return -ENOSYS;
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
	mode_t mode = syscall_arg_raw(1);
	dev_t dev = syscall_arg_raw(2);
	return do_mknodat(AT_FDCWD, path, mode, dev);
}

uint64_t sys_mknodat(void)
{
	int dirfd = syscall_arg_int(0);
	char *path = syscall_arg_ptr(1);
	mode_t mode = syscall_arg_raw(2);
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

uint64_t sys_dup2(void)
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

uint64_t sys_mount(void)
{
	return -ENOSYS;
}

uint64_t sys_umount2(void)
{
	return -ENOSYS;
}

uint64_t sys_lseek(void)
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
	uint8_t *buf;
	size_t size;
	size_t pos;
};

static bool getdents_filldir(struct dir_context *ctx, const char *name,
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
	struct file *fp = NULL;
	void *buf;
	size_t size;
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
	uint8_t *buf;
	size_t size;
	size_t pos;
};

static bool getdents64_filldir(struct dir_context *ctx, const char *name,
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
	struct file *fp = NULL;
	void *buf;
	size_t size;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;

	if (!(fp->f_mode & FMODE_DIR)) {
		log_info("%s(): Not a directory\n", __func__);
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

struct file *do_openat(int dirfd, const char *pathname, int flags, mode_t mode)
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

int do_mkdirat(int dirfd, const char *pathname, mode_t mode)
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

int do_mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev)
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
