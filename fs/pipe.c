#include "aosd/fs.h"
#include <aosd/errno.h>
#include <aosd/lock.h>
#include <aosd/pipe.h>
#include <aosd/sched.h>
#include <aosd/sched_types.h>
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
	(*wf)->f_mode = FMODE_WRITE;
	(*wf)->f_pipe = p;
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
		sched_wake_up(&p->r_idx);
	} else {
		p->readable = false;
		sched_wake_up(&p->w_idx);
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
	struct task *t = current_task();
	uint8_t ch;

	spinlock_acquire(&p->lock);
	while (p->r_idx == p->w_idx && p->writable) {
		if (task_is_killed(t)) {
			spinlock_release(&p->lock);
			return -1;
		}
		sched_sleep(&p->r_idx, &p->lock);
	}
	for (i = 0; i < n; i++) {
		if (p->r_idx == p->w_idx)
			break;
		ch = p->data[p->r_idx];
		((uint8_t *)buf)[i] = ch;
		p->r_idx = (p->r_idx + 1) % PIPE_BUF;
	}
	sched_wake_up(&p->w_idx);
	spinlock_release(&p->lock);
	if (read)
		*read = i;
	return i;
}

int pipe_write(struct pipe *p, const void *buf, size_t n, size_t *written)
{
	size_t i = 0;
	struct task *t = current_task();
	uint8_t ch;

	spinlock_acquire(&p->lock);
	while (i < n) {
		if (!p->readable || task_is_killed(t)) {
			spinlock_release(&p->lock);
			return -1;
		}
		if ((p->w_idx + 1) % PIPE_BUF == p->r_idx) {
			sched_wake_up(&p->r_idx);
			sched_sleep(&p->w_idx, &p->lock);
		} else {
			ch = ((const uint8_t *)buf)[i];
			p->data[p->w_idx] = ch;
			p->w_idx = (p->w_idx + 1) % PIPE_BUF;
			i++;
		}
	}
	sched_wake_up(&p->r_idx);
	spinlock_release(&p->lock);

	if (written)
		*written = i;

	return i;
}
