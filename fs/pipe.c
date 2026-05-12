#include <brk/asm.h>
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fcntl.h>
#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mount.h>
#include <brk/panic.h>
#include <brk/path.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/string.h>
#include <brk/types.h>

#define PIPEFS_MAGIC 0x50495045 /* "PIPE" */
#define PIPE_RING_CAP 4096

struct anon_pipe {
	spinlock_t lock;
	size_t head;
	size_t count;
	unsigned int readers;
	unsigned int writers;
	bool nonblock;
	char rwait;
	char wwait;
	char buf[PIPE_RING_CAP];
};

static struct mount *pipe_mnt;
static struct file_system_type pipefs_fs_type;

static const struct qstr pipe_d_name = {
	.name = "[pipe]",
	.len = 6,
	.hash = 0xed5301a9,
};

static struct dentry *pipefs_lookup(struct inode *dir, struct dentry *dentry,
				    unsigned int flags)
{
	(void)dir;
	(void)dentry;
	(void)flags;
	return NULL;
}

static int pipefs_dir_getattr(const struct path *path, struct stat *st,
			      uint32_t mask, unsigned int flags)
{
	struct inode *inode = path->dentry->d_inode;

	(void)mask;
	(void)flags;
	memset(st, 0, sizeof(*st));
	st->st_ino = inode->i_ino;
	st->st_mode = inode->i_mode;
	st->st_nlink = inode->i_nlink;
	st->st_blksize = PIPE_BUF;
	inode_times_to_stat(inode, st);
	return 0;
}

static const struct inode_operations pipefs_dir_iops = {
	.lookup = pipefs_lookup,
	.getattr = pipefs_dir_getattr,
};

static int pipefs_dir_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t pipefs_dir_read(struct file *file, char *buf, size_t size,
			       loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static ssize_t pipefs_dir_write(struct file *file, const char *buf, size_t size,
				loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static loff_t pipefs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t new_pos = 0;

	(void)file;
	if (whence == SEEK_SET)
		new_pos = offset;
	else if (whence == SEEK_CUR)
		new_pos = file->f_pos + offset;
	else
		return -EINVAL;
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int pipefs_dir_iterate_shared(struct file *file, struct dir_context *ctx)
{
	(void)file;
	(void)ctx;
	return 0;
}

static const struct file_operations pipefs_dir_fops = {
	.open = pipefs_dir_open,
	.read = pipefs_dir_read,
	.write = pipefs_dir_write,
	.llseek = pipefs_dir_llseek,
	.iterate_shared = pipefs_dir_iterate_shared,
};

static void pipefs_evict_inode(struct inode *inode)
{
	if (inode->i_private) {
		kfree(inode->i_private);
		inode->i_private = NULL;
	}
	inode_clear(inode);
}

static void pipefs_put_super(struct super_block *sb)
{
	dentry_put(sb->s_root);
}

static int pipefs_write_inode(struct inode *inode, int sync)
{
	(void)inode;
	(void)sync;
	return 0;
}

static void pipefs_dirty_inode(struct inode *inode, int flags)
{
	(void)inode;
	(void)flags;
}

static int pipefs_sync_fs(struct super_block *sb, int wait)
{
	(void)sb;
	(void)wait;
	return 0;
}

static const struct super_operations pipefs_sops = {
	.put_super = pipefs_put_super,
	.evict_inode = pipefs_evict_inode,
	.write_inode = pipefs_write_inode,
	.dirty_inode = pipefs_dirty_inode,
	.sync_fs = pipefs_sync_fs,
};

static void pipefs_kill_sb(struct super_block *sb)
{
	struct file_system_type *fs_type = sb->s_type;

	spinlock_acquire(&fs_type->fs_lock);
	list_del(&sb->s_instances);
	spinlock_release(&fs_type->fs_lock);

	super_put(sb);
}

static struct dentry *pipefs_mount(struct file_system_type *fs_type, int flags,
				   const char *dev_name, void *data)
{
	struct super_block *sb;
	struct inode *root_inode;
	struct dentry *root_dentry;

	(void)dev_name;
	(void)data;

	sb = alloc_super(fs_type);
	if (!sb)
		return ERR_PTR(-ENOMEM);

	sb->s_blocksize = PAGE_SIZE;
	sb->s_magic = PIPEFS_MAGIC;
	sb->s_flags = (unsigned long)flags;
	sb->s_op = &pipefs_sops;
	sb->s_d_op = &generic_dop;

	root_inode = inode_get_locked(sb, 1);
	if (!root_inode) {
		free_super(sb);
		return ERR_PTR(-ENOMEM);
	}

	if (root_inode->i_state & I_NEW) {
		root_inode->i_mode = S_IFDIR | 0555;
		root_inode->i_op = &pipefs_dir_iops;
		root_inode->i_fop = &pipefs_dir_fops;
		root_inode->i_nlink = 1;
		inode_times_set_all_now(root_inode);
		inode_unlock_new(root_inode);
	}

	root_dentry = dentry_make_root(root_inode);
	if (!root_dentry) {
		inode_put(root_inode);
		free_super(sb);
		return ERR_PTR(-ENOMEM);
	}

	sb->s_root = dentry_dup(root_dentry);

	spinlock_acquire(&fs_type->fs_lock);
	list_add_tail(&sb->s_instances, &fs_type->fs_supers);
	spinlock_release(&fs_type->fs_lock);

	return root_dentry;
}

static int pipe_getattr(const struct path *path, struct stat *st, uint32_t mask,
			unsigned int flags)
{
	struct inode *inode = path->dentry->d_inode;

	(void)mask;
	(void)flags;
	memset(st, 0, sizeof(*st));
	st->st_ino = inode->i_ino;
	st->st_mode = inode->i_mode;
	st->st_nlink = inode->i_nlink;
	st->st_size = 0;
	st->st_blksize = PIPE_BUF;
	inode_times_to_stat(inode, st);
	return 0;
}

static const struct inode_operations pipe_inode_iops = {
	.getattr = pipe_getattr,
};

static int pipe_open(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static int pipe_release(struct inode *inode, struct file *file)
{
	struct anon_pipe *pipe = inode->i_private;

	if (!pipe)
		return 0;

	spinlock_acquire(&pipe->lock);
	if (file->f_mode & FMODE_READ) {
		if (pipe->readers)
			pipe->readers--;
		proc_wake_all(&pipe->wwait);
	}
	if (file->f_mode & FMODE_WRITE) {
		if (pipe->writers)
			pipe->writers--;
		proc_wake_all(&pipe->rwait);
	}
	spinlock_release(&pipe->lock);
	return 0;
}

static ssize_t pipe_read(struct file *file, char *buf, size_t size, loff_t *pos)
{
	struct anon_pipe *pipe = file->f_inode->i_private;
	size_t n, first;
	ssize_t total = 0;

	(void)pos;
	if (!pipe)
		return -EINVAL;

	while (size > 0) {
		spinlock_acquire(&pipe->lock);
		while (pipe->count == 0) {
			if (pipe->writers == 0) {
				spinlock_release(&pipe->lock);
				return total;
			}
			if (pipe->nonblock) {
				spinlock_release(&pipe->lock);
				return total ? total : -EAGAIN;
			}
			if (proc_is_killed(current_process())) {
				spinlock_release(&pipe->lock);
				return total ? total : -EINTR;
			}
			proc_sleep(&pipe->rwait, &pipe->lock);
		}

		n = pipe->count;
		if (n > size)
			n = size;
		first = PIPE_RING_CAP - pipe->head;
		if (n > first) {
			memcpy(buf, pipe->buf + pipe->head, first);
			memcpy(buf + first, pipe->buf, n - first);
		} else {
			memcpy(buf, pipe->buf + pipe->head, n);
		}
		pipe->head = (pipe->head + n) % PIPE_RING_CAP;
		pipe->count -= n;
		spinlock_release(&pipe->lock);

		buf += n;
		size -= n;
		total += (ssize_t)n;
		proc_wake_all(&pipe->wwait);
	}

	return total;
}

static ssize_t pipe_write(struct file *file, const char *buf, size_t size,
			  loff_t *pos)
{
	struct anon_pipe *pipe = file->f_inode->i_private;
	size_t n, first;
	ssize_t total = 0;

	(void)pos;
	if (!pipe)
		return -EINVAL;

	while (size > 0) {
		spinlock_acquire(&pipe->lock);
		if (pipe->readers == 0) {
			spinlock_release(&pipe->lock);
			return total ? total : -EPIPE;
		}
		while (pipe->count == PIPE_RING_CAP) {
			if (pipe->nonblock) {
				spinlock_release(&pipe->lock);
				return total ? total : -EAGAIN;
			}
			if (proc_is_killed(current_process())) {
				spinlock_release(&pipe->lock);
				return total ? total : -EINTR;
			}
			proc_sleep(&pipe->wwait, &pipe->lock);
			if (pipe->readers == 0) {
				spinlock_release(&pipe->lock);
				return total ? total : -EPIPE;
			}
		}

		n = PIPE_RING_CAP - pipe->count;
		if (n > size)
			n = size;
		first = PIPE_RING_CAP -
			((pipe->head + pipe->count) % PIPE_RING_CAP);
		if (n > first) {
			memcpy(pipe->buf + (pipe->head + pipe->count) %
						   PIPE_RING_CAP,
			       buf, first);
			memcpy(pipe->buf, buf + first, n - first);
		} else {
			memcpy(pipe->buf + (pipe->head + pipe->count) %
						   PIPE_RING_CAP,
			       buf, n);
		}
		pipe->count += n;
		spinlock_release(&pipe->lock);

		buf += n;
		size -= n;
		total += (ssize_t)n;
		proc_wake_all(&pipe->rwait);
	}

	return total;
}

static loff_t pipe_llseek(struct file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -ESPIPE;
}

static const struct file_operations pipe_fifo_fops = {
	.open = pipe_open,
	.release = pipe_release,
	.read = pipe_read,
	.write = pipe_write,
	.llseek = pipe_llseek,
};

static unsigned long pipe_alloc_ino(void)
{
	static unsigned long counter = 1;

	return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
}

/**
 * anon_pipe_create() - Allocate a pipe and two file objects (read and write ends)
 * @read_file: Output read end (FMODE_READ)
 * @write_file: Output write end (FMODE_WRITE)
 * @flags: O_NONBLOCK and/or O_CLOEXEC (CLOEXEC accepted; fd cloexec not implemented)
 *
 * Return: 0 on success, negative errno on failure.
 */
int anon_pipe_create(struct file **read_file, struct file **write_file,
		     unsigned int flags)
{
	struct inode *inode;
	struct dentry *d;
	struct path path;
	struct anon_pipe *pipe;
	struct file *rf, *wf;
	unsigned long ino;

	if (!pipe_mnt)
		return -ENODEV;

	(void)(flags & O_CLOEXEC);

	pipe = kzalloc(sizeof(*pipe));
	if (!pipe)
		return -ENOMEM;

	spinlock_init(&pipe->lock, "pipe.lock");
	pipe->readers = 1;
	pipe->writers = 1;
	pipe->nonblock = (flags & O_NONBLOCK) != 0;

	ino = pipe_alloc_ino();
	inode = inode_get_locked(pipe_mnt->mnt_sb, ino);
	if (!inode) {
		kfree(pipe);
		return -ENOMEM;
	}

	if (!(inode->i_state & I_NEW)) {
		inode_put(inode);
		kfree(pipe);
		return -EBUSY;
	}

	inode->i_mode = S_IFIFO | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	inode->i_op = &pipe_inode_iops;
	inode->i_fop = &pipe_fifo_fops;
	inode->i_nlink = 1;
	inode->i_private = pipe;
	inode_times_set_all_now(inode);
	inode_unlock_new(inode);

	d = dentry_alloc_anon(inode, &pipe_d_name);
	if (!d) {
		inode_put(inode);
		return -ENOMEM;
	}

	path.mnt = mount_dup(pipe_mnt);
	path.dentry = d;

	rf = file_alloc(&path, FMODE_READ);
	if (IS_ERR(rf)) {
		int err = PTR_ERR(rf);

		path_put(&path);
		return err;
	}

	path.mnt = mount_dup(pipe_mnt);
	path.dentry = dentry_dup(d);

	wf = file_alloc(&path, FMODE_WRITE);
	if (IS_ERR(wf)) {
		int err = PTR_ERR(wf);

		path_put(&path);
		file_put(rf);
		return err;
	}

	path_put(&path);

	*read_file = rf;
	*write_file = wf;
	return 0;
}

void pipe_fs_init(void)
{
	pipefs_fs_type.name = "pipefs";
	pipefs_fs_type.mount = pipefs_mount;
	pipefs_fs_type.kill_sb = pipefs_kill_sb;
	spinlock_init(&pipefs_fs_type.fs_lock, "pipefs.fs_lock");
	list_init(&pipefs_fs_type.fs_supers);
	list_init(&pipefs_fs_type.fs_list);

	register_filesystem(&pipefs_fs_type);

	pipe_mnt = kernel_mount(&pipefs_fs_type, 0, "", NULL);
	if (IS_ERR(pipe_mnt))
		panic("pipe_fs_init: kernel_mount(pipefs) failed: %d\n",
		      PTR_ERR(pipe_mnt));
}
