#include <arch/asm/page.h>
#include <brk/atomic.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/mount.h>
#include <brk/panic.h>
#include <brk/path.h>
#include <brk/signal.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/wait.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/poll.h>
#include <uapi/stat.h>

#define PIPEFS_MAGIC 0x50495045 /* "PIPE" */
#define PIPE_RING_CAP 4096

struct anon_pipe {
	spinlock_t lock;
	size_t head;
	size_t count;
	unsigned int readers;
	unsigned int writers;
	bool nonblock;
	wait_queue_head_t rwait;
	wait_queue_head_t wwait;
	char buf[PIPE_RING_CAP];
};

static struct mount_state *pipe_mnt;
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

static int pipefs_dir_getattr(struct inode *inode, struct kstat *st, u32 mask,
			      unsigned int flags)
{
	(void)mask;
	(void)flags;
	memset(st, 0, sizeof(*st));
	st->ino = inode->ino;
	st->mode = inode->mode;
	st->nlink = inode->nlink;
	st->blksize = PIPE_BUF;
	inode_times_to_kstat(inode, st);
	return 0;
}

static const struct inode_ops pipefs_dir_iops = {
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
		new_pos = file->pos + offset;
	else
		return -EINVAL;
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int pipefs_dir_iterate_shared(struct file *file,
				     struct dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return 0;
}

static const struct file_ops pipefs_dir_fops = {
	.open = pipefs_dir_open,
	.read = pipefs_dir_read,
	.write = pipefs_dir_write,
	.llseek = pipefs_dir_llseek,
	.iterate_shared = pipefs_dir_iterate_shared,
};

static void pipefs_evict_inode(struct inode *inode)
{
	if (inode->private_data) {
		kfree(inode->private_data);
		inode->private_data = NULL;
	}
	inode_clear(inode);
}

static void pipefs_put_super(struct super_block *sb)
{
	struct file_system_type *type = sb->type;
	spin_lock(&type->lock);
	list_del(&sb->instance);
	spin_unlock(&type->lock);
	dentry_put(sb->root);
}

static int pipefs_write_inode(struct inode *inode, int sync)
{
	(void)inode;
	(void)sync;
	return 0;
}

static const struct super_block_ops pipefs_sops = {
	.put_super = pipefs_put_super,
	.evict_inode = pipefs_evict_inode,
	.write_inode = pipefs_write_inode,
};

int pipefs_mount(struct mount_args *args, struct mount_result *result)
{
	struct super_block *sb;
	struct inode *root_inode;
	struct dentry *root_dentry;

	sb = super_block_alloc(args->type);
	if (!sb)
		return -ENOMEM;

	sb->block_size = PAGE_SIZE;
	sb->magic = PIPEFS_MAGIC;
	sb->flags = args->flags;
	sb->ops = &pipefs_sops;
	sb->default_dops = &generic_dop;

	root_inode = inode_get_locked(sb, 1);
	if (!root_inode) {
		super_block_free(sb);
		return -ENOMEM;
	}

	irqstate_t root_flags = spin_lock_irqsave(&root_inode->lock);
	bool root_is_new = (root_inode->state & I_NEW) != 0;
	spin_unlock_irqrestore(&root_inode->lock, root_flags);

	if (root_is_new) {
		root_inode->mode = S_IFDIR | 0555;
		root_inode->ops = &pipefs_dir_iops;
		root_inode->fops = &pipefs_dir_fops;
		root_inode->nlink = 1;
		inode_times_set_all_now(root_inode);
		inode_unlock_new(root_inode);
	}

	root_dentry = dentry_make_root(root_inode);
	if (!root_dentry) {
		inode_put(root_inode);
		super_block_free(sb);
		return -ENOMEM;
	}

	sb->root = dentry_get(root_dentry);

	spin_lock(&args->type->lock);
	list_add_tail(&sb->instance, &args->type->super_blocks);
	spin_unlock(&args->type->lock);

	result->root = root_dentry;
	result->sb = sb;

	return 0;
}

static int pipe_getattr(struct inode *inode, struct kstat *st, u32 mask,
			unsigned int flags)
{
	(void)mask;
	(void)flags;
	memset(st, 0, sizeof(*st));
	st->ino = inode->ino;
	st->mode = inode->mode;
	st->nlink = inode->nlink;
	st->size = 0;
	st->blksize = PIPE_BUF;
	inode_times_to_kstat(inode, st);
	return 0;
}

static const struct inode_ops pipe_inode_iops = {
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
	struct anon_pipe *pipe = inode->private_data;
	irqstate_t flags;

	if (!pipe)
		return 0;

	flags = spin_lock_irqsave(&pipe->lock);
	if (file->mode & FMODE_READ) {
		if (pipe->readers)
			pipe->readers--;
		wake_up_all(&pipe->wwait);
	}
	if (file->mode & FMODE_WRITE) {
		if (pipe->writers)
			pipe->writers--;
		wake_up_all(&pipe->rwait);
	}
	spin_unlock_irqrestore(&pipe->lock, flags);
	return 0;
}

static ssize_t pipe_read(struct file *file, char *buf, size_t size, loff_t *pos)
{
	struct anon_pipe *pipe = file->inode->private_data;
	size_t n, first;
	ssize_t total = 0;
	irqstate_t flags;

	(void)pos;
	if (!pipe)
		return -EINVAL;

	while (size > 0) {
		flags = spin_lock_irqsave(&pipe->lock);
		while (pipe->count == 0) {
			/*
			 * A read returns as soon as any data is available.
			 * Do not wait for the caller's full buffer to fill.
			 */
			if (total) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total;
			}
			if (pipe->writers == 0) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total;
			}
			if (pipe->nonblock) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total ? total : -EAGAIN;
			}
			if (thread_abort_pending(current_thread())) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total ? total : -EINTR;
			}
			if (thread_sleep_on_interruptible(
				    &pipe->rwait, pipe->rwait.lock) < 0) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total ? total : -EINTR;
			}
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
		spin_unlock_irqrestore(&pipe->lock, flags);

		buf += n;
		size -= n;
		total += (ssize_t)n;
		wake_up_all(&pipe->wwait);
	}

	return total;
}

static ssize_t pipe_write(struct file *file, const char *buf, size_t size,
			  loff_t *pos)
{
	struct anon_pipe *pipe = file->inode->private_data;
	size_t n, first;
	ssize_t total = 0;
	irqstate_t flags;

	(void)pos;
	if (!pipe)
		return -EINVAL;

	while (size > 0) {
		flags = spin_lock_irqsave(&pipe->lock);
		if (pipe->readers == 0) {
			spin_unlock_irqrestore(&pipe->lock, flags);
			return total ? total : -EPIPE;
		}
		while (pipe->count == PIPE_RING_CAP) {
			if (pipe->nonblock) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total ? total : -EAGAIN;
			}
			if (thread_abort_pending(current_thread())) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total ? total : -EINTR;
			}
			if (thread_sleep_on_interruptible(
				    &pipe->wwait, pipe->wwait.lock) < 0) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return total ? total : -EINTR;
			}
			if (pipe->readers == 0) {
				spin_unlock_irqrestore(&pipe->lock, flags);
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
		spin_unlock_irqrestore(&pipe->lock, flags);

		buf += n;
		size -= n;
		total += (ssize_t)n;
		wake_up_all(&pipe->rwait);
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

static unsigned int pipe_poll(struct file *file, unsigned int events)
{
	struct anon_pipe *pipe = file->inode->private_data;
	unsigned int rev = 0;
	irqstate_t flags;

	if (!pipe)
		return POLLERR;

	flags = spin_lock_irqsave(&pipe->lock);
	if (pipe->count > 0)
		rev |= POLLIN | POLLRDNORM;
	if (pipe->writers == 0)
		rev |= POLLHUP;
	if (pipe->count < PIPE_RING_CAP && pipe->readers > 0)
		rev |= POLLOUT | POLLWRNORM;
	if (pipe->readers == 0)
		rev |= POLLERR;
	spin_unlock_irqrestore(&pipe->lock, flags);

	return rev & (events | POLLERR | POLLHUP);
}

static const struct file_ops pipe_fifo_fops = {
	.open = pipe_open,
	.release = pipe_release,
	.read = pipe_read,
	.write = pipe_write,
	.llseek = pipe_llseek,
	.poll = pipe_poll,
};

static unsigned long pipe_alloc_ino(void)
{
	static unsigned long counter = 1;

	return atomic_add_fetch(&counter, 1, ATOMIC_RELAXED);
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
	wait_queue_init(&pipe->rwait, &pipe->lock, "pipe.rwait");
	wait_queue_init(&pipe->wwait, &pipe->lock, "pipe.wwait");
	pipe->readers = 1;
	pipe->writers = 1;
	pipe->nonblock = (flags & O_NONBLOCK) != 0;

	ino = pipe_alloc_ino();
	inode = inode_get_locked(pipe_mnt->sb, ino);
	if (!inode) {
		kfree(pipe);
		return -ENOMEM;
	}

	irqstate_t inode_flags = spin_lock_irqsave(&inode->lock);
	bool inode_is_new = (inode->state & I_NEW) != 0;
	spin_unlock_irqrestore(&inode->lock, inode_flags);

	if (!inode_is_new) {
		inode_put(inode);
		kfree(pipe);
		return -EBUSY;
	}

	inode->mode = S_IFIFO | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	inode->ops = &pipe_inode_iops;
	inode->fops = &pipe_fifo_fops;
	inode->nlink = 1;
	inode->private_data = pipe;
	inode_times_set_all_now(inode);
	inode_unlock_new(inode);

	d = dentry_alloc_anon(inode, &pipe_d_name);
	if (!d) {
		inode_put(inode);
		return -ENOMEM;
	}

	/*
	 * @path owns the dentry/mount references created above.
	 * file_alloc() takes an additional reference for each file, so do
	 * not get them again for the second end: that would leak one dentry
	 * and one mount reference per pipe.
	 */
	path.mnt = mount_state_get(pipe_mnt);
	path.dentry = d;

	rf = file_alloc(&path, FMODE_READ);
	if (IS_ERR(rf)) {
		int err = PTR_ERR(rf);

		path_put(&path);
		return err;
	}

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
	spinlock_init(&pipefs_fs_type.lock, "pipefs.lock");
	list_init(&pipefs_fs_type.super_blocks);
	list_init(&pipefs_fs_type.list);

	file_system_type_register(&pipefs_fs_type);

	pipe_mnt = kernel_mount(&pipefs_fs_type, 0, "", NULL);
	if (IS_ERR(pipe_mnt))
		panic("pipe_fs_init: kernel_mount(pipefs) failed: %d\n",
		      PTR_ERR(pipe_mnt));
}

int do_pipe2(int *pipefd, int flags)
{
	struct file *rf, *wf;
	struct thread *task = current_thread();
	int fd0, fd1, err;

	if (flags & ~(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;

	err = anon_pipe_create(&rf, &wf, (unsigned int)flags);
	if (err)
		return err;

	fd0 = fdtable_alloc_fd(&task->process->fdtable, rf);
	if (fd0 < 0) {
		file_put(wf);
		file_put(rf);
		return -EMFILE;
	}

	fd1 = fdtable_alloc_fd(&task->process->fdtable, wf);
	if (fd1 < 0) {
		fdtable_close_fd(&task->process->fdtable, fd0);
		file_put(wf);
		return -EMFILE;
	}

	pipefd[0] = fd0;
	pipefd[1] = fd1;
	return 0;
}
