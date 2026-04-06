#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/lock.h>
#include <aosd/pipe.h>
#include <aosd/process.h>
#include <aosd/slab.h>

int pipe_alloc(struct file **rf, struct file **wf)
{
	int err;
	struct pipe *p = kmalloc(sizeof(*p));
	if (!p) {
		err = -ENOMEM;
		goto err0;
	}
	*rf = file_alloc();
	if (!*rf) {
		err = -ENOMEM;
		goto err1;
	}
	*wf = file_alloc();
	if (!*wf) {
		err = -ENOMEM;
		goto err2;
	}
	p->readable = true;
	p->writable = true;
	p->r_idx = 0;
	p->w_idx = 0;
	spinlock_init(&p->lock, "pipe");
	(*rf)->f_mode = FMODE_READ;
	(*rf)->f_pipe = p;
	(*rf)->f_ops = &pipe_fops;
	(*wf)->f_mode = FMODE_WRITE;
	(*wf)->f_pipe = p;
	(*wf)->f_ops = &pipe_fops;
	return 0;

err2:
	file_put(*rf);
err1:
	kfree(p);
err0:
	return err;
}

void pipe_close(struct pipe *p, bool writable)
{
	spinlock_acquire(&p->lock);
	if (writable) {
		p->writable = false;
		proc_wake_up(&p->r_idx);
	} else {
		p->readable = false;
		proc_wake_up(&p->w_idx);
	}
	if (!p->readable && !p->writable) {
		spinlock_release(&p->lock);
		kfree(p);
	} else {
		spinlock_release(&p->lock);
	}
}

int pipe_read(struct pipe *p, void *buf, size_t n, size_t *read)
{
	size_t i;
	struct process *proc = current_process();
	uint8_t ch;

	spinlock_acquire(&p->lock);
	while (p->r_idx == p->w_idx && p->writable) {
		if (proc_is_killed(proc)) {
			spinlock_release(&p->lock);
			return -1;
		}
		proc_sleep(&p->r_idx, &p->lock);
	}
	for (i = 0; i < n; i++) {
		if (p->r_idx == p->w_idx)
			break;
		ch = p->data[p->r_idx];
		((uint8_t *)buf)[i] = ch;
		p->r_idx = (p->r_idx + 1) % PIPE_BUF;
	}
	proc_wake_up(&p->w_idx);
	spinlock_release(&p->lock);
	if (read)
		*read = i;
	return i;
}

int pipe_write(struct pipe *p, const void *buf, size_t n, size_t *written)
{
	size_t i = 0;
	struct process *proc = current_process();
	uint8_t ch;

	spinlock_acquire(&p->lock);
	while (i < n) {
		if (!p->readable || proc_is_killed(proc)) {
			spinlock_release(&p->lock);
			return -1;
		}
		if ((p->w_idx + 1) % PIPE_BUF == p->r_idx) {
			proc_wake_up(&p->r_idx);
			proc_sleep(&p->w_idx, &p->lock);
		} else {
			ch = ((const uint8_t *)buf)[i];
			p->data[p->w_idx] = ch;
			p->w_idx = (p->w_idx + 1) % PIPE_BUF;
			i++;
		}
	}
	proc_wake_up(&p->r_idx);
	spinlock_release(&p->lock);

	if (written)
		*written = i;

	return i;
}

static int pipe_fopen(struct file *fp, struct dentry *dentry, int flags)
{
	return -EOPNOTSUPP;
}

static int pipe_fread(struct file *fp, void *buf, size_t cnt, off_t *off,
		      size_t *rcnt)
{
	return pipe_read(fp->f_pipe, buf, cnt, rcnt);
}

static int pipe_fwrite(struct file *fp, const void *buf, size_t cnt, off_t *off,
		       size_t *written)
{
	return pipe_write(fp->f_pipe, buf, cnt, written);
}

static int pipe_fstat(struct file *fp, struct stat *st)
{
	return -EOPNOTSUPP;
}

static off_t pipe_fseek(struct file *fp, off_t offset, int whence)
{
	return -EOPNOTSUPP;
}

static int pipe_ftruncate(struct file *fp, off_t offset)
{
	return -EOPNOTSUPP;
}

const struct file_operations pipe_fops = {
	.open = pipe_fopen,
	.read = pipe_fread,
	.write = pipe_fwrite,
	.stat = pipe_fstat,
	.seek = pipe_fseek,
	.truncate = pipe_ftruncate,
	.close = NULL,
};
