#include <aosd/dcache.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/path.h>
#include <aosd/pipe.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/process_types.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/syscall.h>
#include <aosd/types.h>
#include <uapi/aosd/fcntl.h>
#include <uapi/aosd/stat.h>
#include <uapi/aosd/utsname.h>

uint64_t sys_read(void)
{
	int fd;
	void *buf;
	size_t n;
	struct file *fp;
	size_t r = 0;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;
	if (fp->f_mode & FMODE_DIR)
		return -EISDIR;
	buf = syscall_arg_ptr(1);
	n = syscall_arg_raw(2);
	err = file_read(fp, buf, n, &r);
	if (err)
		return err;
	return r;
}

uint64_t sys_write(void)
{
	int fd;
	const void *buf;
	size_t n;
	struct file *fp;
	size_t w = 0;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;
	if (fp->f_mode & FMODE_DIR)
		return -EISDIR;
	buf = syscall_arg_ptr(1);
	n = syscall_arg_raw(2);
	err = file_write(fp, buf, n, &w);
	if (err)
		return err;
	return w;
}

uint64_t sys_open(void)
{
	int fd;
	int err;
	struct file *fp = NULL;
	char *path = syscall_arg_ptr(0);
	int flags = syscall_arg_raw(1);
	mode_t mode = syscall_arg_raw(2);

	err = do_openat(AT_FDCWD, path, flags, mode, &fp);
	if (err)
		return err;

	fd = proc_alloc_fd(proc_get_current(), fp);
	if (fd >= 0) {
		return fd;
	} else {
		file_put(fp);
		return -EMFILE;
	}
}

uint64_t sys_openat(void)
{
	int fd;
	int err;
	struct file *fp = NULL;
	int dirfd = syscall_arg_raw(0);
	char *path = syscall_arg_ptr(1);
	int flags = syscall_arg_raw(2);
	mode_t mode = syscall_arg_raw(3);

	err = do_openat(dirfd, path, flags, mode, &fp);
	if (err)
		return err;

	fd = proc_alloc_fd(proc_get_current(), fp);
	if (fd >= 0) {
		return fd;
	} else {
		file_put(fp);
		return -EMFILE;
	}
}

uint64_t sys_close(void)
{
	int err;
	struct file *fp = NULL;
	int fd = 0;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;

	proc_get_current()->ofiles[fd] = NULL;
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
	return -EOPNOTSUPP;
}

uint64_t sys_munmap(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_mprotect(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_msync(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_mremap(void)
{
	return -EOPNOTSUPP;
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
	return -EOPNOTSUPP;
}

uint64_t sys_stat(void)
{
	int err;
	const char *path = syscall_arg_ptr(0);
	struct stat *buf = syscall_arg_ptr(1);
	struct file *fp = NULL;

	err = do_openat(AT_FDCWD, path, O_RDONLY, 0, &fp);
	if (err)
		return err;

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
	return -EOPNOTSUPP;
}

uint64_t sys_readlink(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_rename(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_creat(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_rmdir(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_uname(void)
{
	struct utsname name = {
		.sysname = "aosd",
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
	struct process *proc;
	char *path;
	size_t len;
	char *buf = syscall_arg_ptr(0);
	size_t size = syscall_arg_raw(1);

	proc = proc_get_current();
	path = path_get_full(proc->cwd);
	len = strlen(path);
	if (len + 1 > size) {
		kfree(path);
		return -ERANGE;
	}

	memcpy(buf, path, len + 1);
	kfree(path);
	return 0;
}

uint64_t sys_chdir(void)
{
	char *path = syscall_arg_ptr(0);
	struct process *proc = proc_get_current();
	struct dentry *new_cwd = path_lookup(path);
	if (!new_cwd)
		return -ENOENT;
	if (!new_cwd->d_inode || !(new_cwd->d_inode->i_mode & S_IFDIR)) {
		dentry_put(new_cwd);
		return -ENOTDIR;
	}
	struct dentry *old_cwd = proc->cwd;
	proc->cwd = new_cwd;
	dentry_put(old_cwd);
	return 0;
}

uint64_t sys_fchdir(void)
{
	int err;
	struct dentry *new_cwd, *old_cwd;
	char *path;
	int fd = 0;
	struct file *fp = NULL;
	struct process *proc = proc_get_current();

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;

	if (!fp->f_inode)
		return -EBADF;

	path = syscall_arg_ptr(1);
	new_cwd = path_lookup_at(fp->f_dentry, path);
	if (!new_cwd)
		return -ENOENT;
	if (!new_cwd->d_inode || !(new_cwd->d_inode->i_mode & S_IFDIR)) {
		dentry_put(new_cwd);
		return -ENOTDIR;
	}

	old_cwd = proc->cwd;
	proc->cwd = new_cwd;
	dentry_put(old_cwd);
	return 0;
}

uint64_t sys_renameat(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_symlinkat(void)
{
	return -EOPNOTSUPP;
}

uint64_t sys_readlinkat(void)
{
	return -EOPNOTSUPP;
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
	int *pipefd;
	int fd0 = -1;
	int fd1 = -1;
	struct file *rf = NULL;
	struct file *wf = NULL;
	struct process *proc = proc_get_current();
	int err;

	err = pipe_alloc(&rf, &wf);
	if (err)
		return -ENOMEM;

	fd0 = proc_alloc_fd(proc, rf);
	if (fd0 < 0) {
		err = -EMFILE;
		goto err0;
	}
	fd1 = proc_alloc_fd(proc, wf);
	if (fd1 < 0) {
		err = -EMFILE;
		goto err1;
	}

	pipefd = syscall_arg_ptr(0);
	pipefd[0] = fd0;
	pipefd[1] = fd1;

	return 0;

err1:
	proc->ofiles[fd0] = NULL;
err0:
	file_put(rf);
	file_put(wf);
	return err;
}

uint64_t sys_pipe2(void)
{
	int *pipefd;
	int fd0 = -1;
	int fd1 = -1;
	struct file *rf = NULL;
	struct file *wf = NULL;
	struct process *proc = proc_get_current();
	int err;

	err = pipe_alloc(&rf, &wf);
	if (err)
		return -ENOMEM;

	fd0 = proc_alloc_fd(proc, rf);
	if (fd0 < 0) {
		err = -EMFILE;
		goto err0;
	}
	fd1 = proc_alloc_fd(proc, wf);
	if (fd1 < 0) {
		err = -EMFILE;
		goto err1;
	}

	pipefd = syscall_arg_ptr(0);
	pipefd[0] = fd0;
	pipefd[1] = fd1;

	return 0;

err1:
	proc->ofiles[fd0] = NULL;
err0:
	file_put(rf);
	file_put(wf);
	return err;
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
	newfd = proc_alloc_fd(proc_get_current(), fp);
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
	struct process *proc = proc_get_current();
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
	return -EOPNOTSUPP;
}

uint64_t sys_umount2(void)
{
	return -EOPNOTSUPP;
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

	return file_seek(fp, off, whence);
}

uint64_t sys_getdents64(void)
{
	void *buf;
	size_t buf_size;
	int fd = -1;
	struct file *fp = NULL;
	size_t rcnt = 0;
	int err;

	err = syscall_arg_fd(0, &fd, &fp);
	if (err)
		return err;
	buf = syscall_arg_ptr(1);
	buf_size = syscall_arg_raw(2);

	err = file_read(fp, buf, buf_size, &rcnt);
	if (err)
		return err;
	return rcnt;
}

static struct dentry *path_to_dentry(const char *path)
{
	if (path[0] == '/')
		return dentry_get(NULL, "/");
	if (path[0] == '.' && path[1] == '/')
		return dentry_dup(proc_get_current()->cwd);
	return NULL;
}

static struct dentry *fd_to_dentry(int fd)
{
	struct process *proc = proc_get_current();

	if (fd == AT_FDCWD)
		return dentry_dup(proc->cwd);

	if (0 <= fd && fd < OPEN_MAX && proc->ofiles[fd] &&
	    proc->ofiles[fd]->f_dentry)
		return dentry_dup(proc->ofiles[fd]->f_dentry);

	return NULL;
}

int do_openat(int dirfd, const char *path, int flags, mode_t mode,
	      struct file **file)
{
	char buf[NAME_MAX] = { 0 };
	struct file *fp;
	struct dentry *parent_dp;
	struct dentry *file_dp;
	struct dentry *dir_dp;
	int err;

	if ((flags & O_RDWR) && (flags & O_WRONLY))
		return -EINVAL;

	dir_dp = path_to_dentry(path);
	if (!dir_dp) {
		dir_dp = fd_to_dentry(dirfd);
		if (!dir_dp)
			return -EBADF;
	}

	file_dp = path_lookup_at(dir_dp, path);
	if (!file_dp && !(flags & O_CREAT)) {
		dentry_put(dir_dp);
		return -ENOENT;
	}

	if (!file_dp && (flags & O_CREAT)) {
		parent_dp = path_lookup_parent_at(dir_dp, path, buf, NAME_MAX);
		if (!parent_dp) {
			dentry_put(dir_dp);
			return -ENOENT;
		}

		file_dp = dentry_alloc(buf, strlen(buf));
		if (!file_dp) {
			dentry_put(parent_dp);
			dentry_put(dir_dp);
			return -ENOMEM;
		}

		err = parent_dp->d_inode->i_ops->create(parent_dp, file_dp,
							mode);
		if (err) {
			dentry_free(file_dp);
			dentry_put(parent_dp);
			dentry_put(dir_dp);
			return err;
		}
		file_dp->d_parent = parent_dp;
		dentry_add(file_dp);
	}

	dentry_put(dir_dp);

	fp = file_alloc();
	if (!fp) {
		dentry_put(file_dp);
		return -ENOMEM;
	}

	if (flags == O_RDONLY)
		fp->f_mode = FMODE_READ;

	if (flags & O_WRONLY)
		fp->f_mode = FMODE_WRITE;

	if (flags & O_RDWR)
		fp->f_mode = FMODE_READ | FMODE_WRITE;

	if (inode_mode(file_dp->d_inode) & S_IFDIR)
		fp->f_mode |= FMODE_DIR;

	err = file_dp->d_inode->i_fops->open(fp, file_dp, flags);
	if (err) {
		file_put(fp);
		dentry_put(file_dp);
		return err;
	}

	dentry_put(file_dp);

	if (flags & O_TRUNC) {
		err = file_truncate(fp, 0);
		if (err) {
			file_put(fp);
			return err;
		}
	}

	if (flags & O_APPEND) {
		off_t ret = file_seek(fp, 0, SEEK_END);
		if (ret < 0) {
			file_put(fp);
			return err;
		}
	}

	*file = fp;

	return 0;
}

int do_mkdirat(int dirfd, const char *path, mode_t mode)
{
	char buf[NAME_MAX] = { 0 };
	struct dentry *dir_dp, *parent_dp, *new_dp;
	int err;

	dir_dp = path_to_dentry(path);
	if (!dir_dp) {
		dir_dp = fd_to_dentry(dirfd);
		if (!dir_dp)
			return -EBADF;
	}

	parent_dp = path_lookup_parent_at(dir_dp, path, buf, NAME_MAX);
	dentry_put(dir_dp);
	if (!parent_dp)
		return -ENOENT;

	new_dp = dentry_alloc(buf, strlen(buf));
	if (!new_dp) {
		dentry_put(parent_dp);
		return -ENOMEM;
	}

	err = parent_dp->d_inode->i_ops->mkdir(parent_dp, new_dp, mode);
	if (err) {
		dentry_free(new_dp);
		dentry_put(parent_dp);
		return err;
	}

	new_dp->d_parent = parent_dp;
	dentry_add(new_dp);
	dentry_put(new_dp);
	return 0;
}

int do_linkat(int olddirfd, const char *oldpath, int newdirfd,
	      const char *newpath, int flags)
{
	char buf[NAME_MAX] = { 0 };
	struct dentry *old_dir_dp, *old_dp;
	struct dentry *new_dir_dp, *parent_dp, *new_dp;
	int err;

	old_dir_dp = path_to_dentry(oldpath);
	if (!old_dir_dp) {
		old_dir_dp = fd_to_dentry(olddirfd);
		if (!old_dir_dp) {
			err = -EBADF;
			goto err0;
		}
	}

	new_dir_dp = path_to_dentry(newpath);
	if (!new_dir_dp) {
		new_dir_dp = fd_to_dentry(newdirfd);
		if (!new_dir_dp) {
			err = -EBADF;
			goto err1;
		}
	}

	old_dp = path_lookup_at(old_dir_dp, oldpath);
	if (!old_dp) {
		err = -ENOENT;
		goto err2;
	}

	parent_dp = path_lookup_parent_at(new_dir_dp, newpath, buf, NAME_MAX);
	if (!parent_dp) {
		err = -ENOENT;
		goto err3;
	}

	new_dp = dentry_alloc(buf, strlen(buf));
	if (!new_dp) {
		err = -ENOMEM;
		goto err4;
	}

	err = parent_dp->d_inode->i_ops->link(old_dp, parent_dp, new_dp);
	if (err)
		goto err5;
	new_dp->d_parent = parent_dp;
	dentry_add(new_dp);
	dentry_put(new_dp);
	dentry_put(old_dp);
	dentry_put(new_dir_dp);
	dentry_put(old_dir_dp);
	return 0;

err5:
	dentry_free(new_dp);
err4:
	dentry_put(parent_dp);
err3:
	dentry_put(old_dp);
err2:
	dentry_put(new_dir_dp);
err1:
	dentry_put(old_dir_dp);
err0:
	return err;
}

int do_unlinkat(int dirfd, const char *path, int flags)
{
	struct dentry *dir_dp, *parent_dp, *old_dp;
	int ret;

	dir_dp = path_to_dentry(path);
	if (!dir_dp) {
		dir_dp = fd_to_dentry(dirfd);
		if (!dir_dp)
			return -EBADF;
	}

	old_dp = path_lookup_at(dir_dp, path);
	if (!old_dp) {
		dentry_put(dir_dp);
		return -ENOENT;
	}

	if (old_dp->d_rc > 1 || old_dp->d_inode->i_rc > 1) {
		dentry_put(old_dp);
		dentry_put(dir_dp);
		return -EBUSY;
	}

	parent_dp = dentry_dup(old_dp->d_parent);

	ret = parent_dp->d_inode->i_ops->unlink(parent_dp, old_dp);

	dentry_put(parent_dp);
	dentry_put(old_dp);
	dentry_put(dir_dp);

	return ret;
}

int do_mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
	char buf[NAME_MAX] = { 0 };
	struct dentry *dir_dp, *parent_dp, *new_dp;
	int err;

	dir_dp = path_to_dentry(path);
	if (!dir_dp) {
		dir_dp = fd_to_dentry(dirfd);
		if (!dir_dp)
			return -EBADF;
	}

	parent_dp = path_lookup_parent_at(dir_dp, path, buf, NAME_MAX);
	dentry_put(dir_dp);
	if (!parent_dp)
		return -ENOENT;

	new_dp = dentry_alloc(buf, strlen(buf));
	if (!new_dp) {
		dentry_put(parent_dp);
		return -ENOMEM;
	}

	err = parent_dp->d_inode->i_ops->mknod(parent_dp, new_dp, mode, dev);
	if (err) {
		dentry_free(new_dp);
		dentry_put(parent_dp);
		return err;
	}

	new_dp->d_parent = parent_dp;
	dentry_add(new_dp);
	dentry_put(new_dp);
	return 0;
}
