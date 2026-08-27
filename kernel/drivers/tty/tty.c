#include <brk/base/types.h>
#include <brk/drivers/device.h>
#include <brk/drivers/tty.h>
#include <brk/lib/refcnt.h>
#include <brk/lib/string.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <brk/process/signal.h>
#include <brk/process/task.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/ioctl.h>
#include <uapi/brk/signal.h>
#include <uapi/brk/types.h>

#define CTRL(x) ((x) - '@')

#define TTY_DEFAULT_ROWS 24
#define TTY_DEFAULT_COLS 80

static void tty_init_winsize(struct tty *tty)
{
	tty->winsize.ws_row = TTY_DEFAULT_ROWS;
	tty->winsize.ws_col = TTY_DEFAULT_COLS;
	tty->winsize.ws_xpixel = 0;
	tty->winsize.ws_ypixel = 0;
}

struct tty *tty_alloc(void)
{
	struct tty *tty = kzalloc(sizeof(*tty));
	if (!tty)
		return NULL;
	tty->rx_buf = kzalloc(TTY_RX_BUF_SIZE);
	if (!tty->rx_buf) {
		kfree(tty);
		return NULL;
	}
	tty->rx_size = TTY_RX_BUF_SIZE;
	refcnt_init(&tty->refcnt, 0);
	return tty;
}

void tty_free(struct tty *tty)
{
	kfree(tty->rx_buf);
	kfree(tty);
}

void tty_init(struct tty *tty, struct tty_port *port)
{
	tty->port = port;
	tty_init_winsize(tty);
}

void tty_set_foreground(struct tty *tty, struct task_control_block *task)
{
	if (!tty || !tty->port)
		return;
	spinlock_acquire(&tty->port->lock);
	tty->port->foreground = task;
	spinlock_release(&tty->port->lock);
}

long tty_ioctl(struct tty *tty, unsigned int cmd, unsigned long arg)
{
	struct winsize *ws = (struct winsize *)arg;
	struct task_control_block *fg;

	if (!tty)
		return -EBADF;
	if (!ws)
		return -EFAULT;

	tty_set_foreground(tty, current_task());

	switch (cmd) {
	case TIOCGWINSZ:
		*ws = tty->winsize;
		return 0;
	case TIOCSWINSZ:
		tty->winsize = *ws;
		spinlock_acquire(&tty->port->lock);
		fg = tty->port->foreground;
		spinlock_release(&tty->port->lock);
		if (fg)
			signal_send(fg, SIGWINCH);
		return 0;
	default:
		return -ENOTTY;
	}
}

ssize_t tty_read(struct tty *tty, void *buf, size_t n)
{
	size_t target;
	int c;
	char *dst = buf;

	tty_set_foreground(tty, current_task());

	target = n;
	spinlock_acquire(&tty->port->lock);
	while (n > 0) {
		/* wait until interrupt handler has put some
		 * input into cons.buffer. */
		while (tty->rx_r == tty->rx_w) {
			if (signal_pending(current_task())) {
				spinlock_release(&tty->port->lock);
				return -EINTR;
			}
			task_sleep(&tty->rx_r, &tty->port->lock);
		}

		c = tty->rx_buf[tty->rx_r++ % tty->rx_size];

		if (c == CTRL('D')) { /* end-of-file */
			if (n < target) {
				/* Save ^D for next time, to make sure
				 * caller gets a 0-byte result. */
				tty->rx_r--;
			}
			break;
		}

		*dst++ = c;
		--n;

		if (c == '\n') {
			/* a whole line has arrived, return to
			 * the user-level read(). */
			break;
		}
	}
	spinlock_release(&tty->port->lock);

	return target - n;
}

ssize_t tty_write(struct tty *tty, const void *buf, size_t n)
{
	const struct tty_ops *ops = tty->port->driver->ops;
	const uint8_t *p = buf;
	const uint8_t *end = p + n;
	int err = 0;

	spinlock_acquire(&tty->port->lock);
	while (p < end) {
		err = ops->put_char(tty, *p);
		if (err)
			break;
		p++;
	}
	spinlock_release(&tty->port->lock);
	return err ? err : p - (const uint8_t *)buf;
}

void tty_receive(struct tty *tty, int c)
{
	spinlock_acquire(&tty->port->lock);
	const struct tty_ops *ops = tty->port->driver->ops;

	switch (c) {
	case CTRL('C'):
		if (tty->port->foreground)
			signal_send(tty->port->foreground, SIGINT);
		break;
	case CTRL('U'): /* Kill line. */
		while (tty->rx_e != tty->rx_w &&
		       tty->rx_buf[(tty->rx_e - 1) % tty->rx_size] != '\n') {
			tty->rx_e--;
			ops->put_char(tty, TTY_VIS_BACKSPACE);
		}
		break;
	case CTRL('H'): /* Backspace */
	case '\x7f': /* Delete key */
		if (tty->rx_e != tty->rx_w) {
			tty->rx_e--;
			ops->put_char(tty, TTY_VIS_BACKSPACE);
		}
		break;
	default:
		if (c != 0 && tty->rx_e - tty->rx_r < tty->rx_size) {
			c = (c == '\r') ? '\n' : c;

			ops->put_char(tty, c);

			tty->rx_buf[tty->rx_e++ % tty->rx_size] = c;

			if (c == '\n' || c == CTRL('D') ||
			    tty->rx_e - tty->rx_r == tty->rx_size) {
				tty->rx_w = tty->rx_e;
				task_wake_all(&tty->rx_r);
			}
		}
		break;
	}

	spinlock_release(&tty->port->lock);
}

struct tty *tty_attach_port(struct tty_port *port)
{
	struct tty *tty;

	if (!port)
		return NULL;

	spinlock_acquire(&port->lock);
	if (port->tty) {
		spinlock_release(&port->lock);
		return port->tty;
	}

	tty = tty_alloc();
	if (!tty) {
		spinlock_release(&port->lock);
		return NULL;
	}

	tty_init(tty, port);
	port->tty = tty;
	spinlock_release(&port->lock);
	return tty;
}

void tty_detach_port(struct tty_port *port)
{
	struct tty *tty;

	if (!port)
		return;

	spinlock_acquire(&port->lock);
	tty = port->tty;
	port->tty = NULL;
	spinlock_release(&port->lock);

	if (tty)
		tty_free(tty);
}

struct tty *tty_open(struct tty_port *port)
{
	struct tty *tty;

	if (!port)
		return NULL;

	spinlock_acquire(&port->lock);
	tty = port->tty;
	if (!tty) {
		spinlock_release(&port->lock);
		return NULL;
	}
	refcnt_inc(&tty->refcnt);
	spinlock_release(&port->lock);
	return tty;
}

void tty_close(struct tty *tty)
{
	if (!tty)
		return;
	refcnt_dec(&tty->refcnt);
}
