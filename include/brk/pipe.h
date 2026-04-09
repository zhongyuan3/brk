#ifndef BRK_PIPE_H
#define BRK_PIPE_H

#include <brk/fs.h>
#include <brk/limits.h>

struct pipe {
	spinlock_t lock;
	uint8_t data[PIPE_BUF];
	size_t r_idx;
	size_t w_idx;
	bool readable;
	bool writable;
};

int pipe_alloc(struct file **rf, struct file **wf);
void pipe_close(struct pipe *p, bool writable);
int pipe_read(struct pipe *p, void *buf, size_t n, size_t *read);
int pipe_write(struct pipe *p, const void *buf, size_t n, size_t *written);

#endif
