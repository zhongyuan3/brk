#include <brk/asm.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mount.h>
#include <brk/panic.h>
#include <brk/path.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

#define PIPEFS_MAGIC 0x50495045 /* "PIPE" */
#define PIPE_RING_CAP 4096

struct anon_pipe {
	spinlock_t lock;
	usize_t head;
	usize_t count;
	unsigned int readers;
	unsigned int writers;
	bool nonblock;
	char rwait;
	char wwait;
	char buf[PIPE_RING_CAP];
};

static struct fs_mount_state *pipe_mnt;
static struct fs_driver pipefs_fs_type;

static const struct qstr pipe_d_name = {
	.name = "[pipe]",
	.len = 6,
	.hash = 0xed5301a9,
};

static struct fs_dentry *pipefs_lookup(struct fs_inode *dir,
				       struct fs_dentry *dentry,
				       unsigned int flags)
{
	(void)dir;
	(void)dentry;
	(void)flags;
	return NULL;
}

static int pipefs_dir_getattr(const struct fs_path *path, struct stat *st,
			      u32 mask, unsigned int flags)
{
	struct fs_inode *inode = path->dentry->inode;

	(void)mask;
	(void)flags;
	memset(st, 0, sizeof(*st));
	st->st_ino = inode->ino;
	st->st_mode = inode->mode;
	st->st_nlink = inode->nlink;
	st->st_blksize = PIPE_BUF;
	inode_times_to_stat(inode, st);
	return 0;
}

static const struct fs_inode_ops pipefs_dir_iops = {
	.lookup = pipefs_lookup,
	.getattr = pipefs_dir_getattr,
};

static int pipefs_dir_open(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t pipefs_dir_read(struct fs_file *file, char *buf, usize_t size,
			       loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static ssize_t pipefs_dir_write(struct fs_file *file, const char *buf,
				usize_t size, loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static loff_t pipefs_dir_llseek(struct fs_file *file, loff_t offset, int whence)
{
	loff_t new_pos = 0;

	(void)file;
	if (whence == SEEK_SET)
		new_pos = offset;
	else if (whence == SEEK_CUR)
		new_pos = file->pos + offset;
	else
		return -EINVAL;
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int pipefs_dir_iterate_shared(struct fs_file *file,
				     struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return 0;
}

static const struct fs_file_ops pipefs_dir_fops = {
	.open = pipefs_dir_open,
	.read = pipefs_dir_read,
	.write = pipefs_dir_write,
	.llseek = pipefs_dir_llseek,
	.iterate_shared = pipefs_dir_iterate_shared,
};

static void pipefs_evict_inode(struct fs_inode *inode)
{
	if (inode->private_data) {
		kfree(inode->private_data);
		inode->private_data = NULL;
	}
	fs_inode_clear(inode);
}

static void pipefs_put_super(struct fs_super_block *sb)
{
	fs_dentry_put(sb->root);
}

static int pipefs_write_inode(struct fs_inode *inode, int sync)
{
	(void)inode;
	(void)sync;
	return 0;
}

static void pipefs_dirty_inode(struct fs_inode *inode, int flags)
{
	(void)inode;
	(void)flags;
}

static int pipefs_sync_fs(struct fs_super_block *sb, int wait)
{
	(void)sb;
	(void)wait;
	return 0;
}

static const struct fs_super_block_ops pipefs_sops = {
	.put_super = pipefs_put_super,
	.evict_inode = pipefs_evict_inode,
	.write_inode = pipefs_write_inode,
	.dirty_inode = pipefs_dirty_inode,
	.sync_fs = pipefs_sync_fs,
};

static void pipefs_kill_sb(struct fs_super_block *sb)
{
	struct fs_driver *fs_type = sb->driver;

	spinlock_acquire(&fs_type->lock);
	list_del(&sb->instance);
	spinlock_release(&fs_type->lock);

	fs_super_block_put(sb);
}

static struct fs_dentry *pipefs_mount(struct fs_driver *fs_type, int flags,
				      const char *dev_name, void *data)
{
	struct fs_super_block *sb;
	struct fs_inode *root_inode;
	struct fs_dentry *root_dentry;

	(void)dev_name;
	(void)data;

	sb = fs_super_block_alloc(fs_type);
	if (!sb)
		return ERR_PTR(-ENOMEM);

	sb->block_size = PAGE_SIZE;
	sb->magic = PIPEFS_MAGIC;
	sb->flags = (unsigned long)flags;
	sb->ops = &pipefs_sops;
	sb->default_dops = &generic_dop;

	root_inode = fs_inode_get_locked(sb, 1);
	if (!root_inode) {
		fs_super_block_free(sb);
		return ERR_PTR(-ENOMEM);
	}

	if (root_inode->state & I_NEW) {
		root_inode->mode = S_IFDIR | 0555;
		root_inode->ops = &pipefs_dir_iops;
		root_inode->fops = &pipefs_dir_fops;
		root_inode->nlink = 1;
		inode_times_set_all_now(root_inode);
		fs_inode_unlock_new(root_inode);
	}

	root_dentry = fs_dentry_make_root(root_inode);
	if (!root_dentry) {
		fs_inode_put(root_inode);
		fs_super_block_free(sb);
		return ERR_PTR(-ENOMEM);
	}

	sb->root = fs_dentry_get(root_dentry);

	spinlock_acquire(&fs_type->lock);
	list_add_tail(&sb->instance, &fs_type->super_blocks);
	spinlock_release(&fs_type->lock);

	return root_dentry;
}

static int pipe_getattr(const struct fs_path *path, struct stat *st, u32 mask,
			unsigned int flags)
{
	struct fs_inode *inode = path->dentry->inode;

	(void)mask;
	(void)flags;
	memset(st, 0, sizeof(*st));
	st->st_ino = inode->ino;
	st->st_mode = inode->mode;
	st->st_nlink = inode->nlink;
	st->st_size = 0;
	st->st_blksize = PIPE_BUF;
	inode_times_to_stat(inode, st);
	return 0;
}

static const struct fs_inode_ops pipe_inode_iops = {
	.getattr = pipe_getattr,
};

static int pipe_open(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static int pipe_release(struct fs_inode *inode, struct fs_file *file)
{
	struct anon_pipe *pipe = inode->private_data;

	if (!pipe)
		return 0;

	spinlock_acquire(&pipe->lock);
	if (file->mode & FMODE_READ) {
		if (pipe->readers)
			pipe->readers--;
		proc_wake_all(&pipe->wwait);
	}
	if (file->mode & FMODE_WRITE) {
		if (pipe->writers)
			pipe->writers--;
		proc_wake_all(&pipe->rwait);
	}
	spinlock_release(&pipe->lock);
	return 0;
}

static ssize_t pipe_read(struct fs_file *file, char *buf, usize_t size,
			 loff_t *pos)
{
	struct anon_pipe *pipe = file->inode->private_data;
	usize_t n, first;
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

static ssize_t pipe_write(struct fs_file *file, const char *buf, usize_t size,
			  loff_t *pos)
{
	struct anon_pipe *pipe = file->inode->private_data;
	usize_t n, first;
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

static loff_t pipe_llseek(struct fs_file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -ESPIPE;
}

static const struct fs_file_ops pipe_fifo_fops = {
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
int anon_pipe_create(struct fs_file **read_file, struct fs_file **write_file,
		     unsigned int flags)
{
	struct fs_inode *inode;
	struct fs_dentry *d;
	struct fs_path path;
	struct anon_pipe *pipe;
	struct fs_file *rf, *wf;
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
	inode = fs_inode_get_locked(pipe_mnt->sb, ino);
	if (!inode) {
		kfree(pipe);
		return -ENOMEM;
	}

	if (!(inode->state & I_NEW)) {
		fs_inode_put(inode);
		kfree(pipe);
		return -EBUSY;
	}

	inode->mode = S_IFIFO | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	inode->ops = &pipe_inode_iops;
	inode->fops = &pipe_fifo_fops;
	inode->nlink = 1;
	inode->private_data = pipe;
	inode_times_set_all_now(inode);
	fs_inode_unlock_new(inode);

	d = fs_dentry_alloc_anon(inode, &pipe_d_name);
	if (!d) {
		fs_inode_put(inode);
		return -ENOMEM;
	}

	path.mnt = fs_mount_state_get(pipe_mnt);
	path.dentry = d;

	rf = fs_file_alloc(&path, FMODE_READ);
	if (IS_ERR(rf)) {
		int err = PTR_ERR(rf);

		fs_path_put(&path);
		return err;
	}

	path.mnt = fs_mount_state_get(pipe_mnt);
	path.dentry = fs_dentry_get(d);

	wf = fs_file_alloc(&path, FMODE_WRITE);
	if (IS_ERR(wf)) {
		int err = PTR_ERR(wf);

		fs_path_put(&path);
		fs_file_put(rf);
		return err;
	}

	fs_path_put(&path);

	*read_file = rf;
	*write_file = wf;
	return 0;
}

void pipe_fs_init(void)
{
	pipefs_fs_type.name = "pipefs";
	pipefs_fs_type.mount = pipefs_mount;
	pipefs_fs_type.kill_sb = pipefs_kill_sb;
	spinlock_init(&pipefs_fs_type.lock, "pipefs.lock");
	list_init(&pipefs_fs_type.super_blocks);
	list_init(&pipefs_fs_type.list);

	fs_driver_register(&pipefs_fs_type);

	pipe_mnt = kernel_mount(&pipefs_fs_type, 0, "", NULL);
	if (IS_ERR(pipe_mnt))
		panic("pipe_fs_init: kernel_mount(pipefs) failed: %d\n",
		      PTR_ERR(pipe_mnt));
}

int do_pipe2(int *pipefd, int flags)
{
	struct fs_file *rf, *wf;
	struct process *proc = current_process();
	int fd0, fd1, err;

	if (flags & ~(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;

	err = anon_pipe_create(&rf, &wf, (unsigned int)flags);
	if (err)
		return err;

	fd0 = proc_alloc_fd(proc, rf);
	if (fd0 < 0) {
		fs_file_put(wf);
		fs_file_put(rf);
		return -EMFILE;
	}

	fd1 = proc_alloc_fd(proc, wf);
	if (fd1 < 0) {
		proc->ofiles[fd0] = NULL;
		fs_file_put(wf);
		fs_file_put(rf);
		return -EMFILE;
	}

	pipefd[0] = fd0;
	pipefd[1] = fd1;
	return 0;
}
