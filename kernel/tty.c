#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/process.h>
#include <brk/string.h>
#include <brk/tty.h>

#define CTRL(x) ((x) - '@')

static struct tty boot_tty;
static struct tty *tty_chrdev_tty;

static void tty_flush_input(struct tty *tty)
{
	tty->rx_r = 0;
	tty->rx_w = 0;
	tty->rx_e = 0;
}

void tty_boot_init(struct tty_port *port)
{
	tty_init(&boot_tty, port);
}

struct tty *tty_boot(void)
{
	return &boot_tty;
}

void tty_init(struct tty *tty, struct tty_port *port)
{
	spinlock_init(&tty->lock, "tty");
	tty->port = port;
	if (port)
		port->tty = tty;
	tty_flush_input(tty);
}

static void tty_echo(struct tty *tty, int c)
{
	if (!tty->port || !tty->port->put_char)
		return;

	if (c == TTY_VIS_BACKSPACE) {
		tty->port->put_char(tty->port, TTY_VIS_BACKSPACE);
		return;
	}

	tty->port->put_char(tty->port, c);
}

int tty_read(struct tty *tty, char *buf, usize_t n, usize_t *read)
{
	usize_t target = n;
	int c;

	spinlock_acquire(&tty->lock);
	while (n > 0) {
		while (tty->rx_r == tty->rx_w) {
			if (proc_is_killed(current_process())) {
				spinlock_release(&tty->lock);
				return -1;
			}
			proc_sleep(&tty->rx_r, &tty->lock);
		}

		c = tty->rx_buf[tty->rx_r++ % TTY_RX_BUF_SIZE];

		if (c == CTRL('D')) {
			if (n < target)
				tty->rx_r--;
			break;
		}

		*buf++ = c;
		--n;

		if (c == '\n')
			break;
	}
	if (read)
		*read = target - n;
	spinlock_release(&tty->lock);
	return 0;
}

int tty_write(struct tty *tty, const char *buf, usize_t n, usize_t *written)
{
	if (!tty->port || !tty->port->put_char) {
		if (written)
			*written = 0;
		return 0;
	}

	for (usize_t i = 0; i < n; ++i)
		tty->port->put_char(tty->port, (unsigned char)buf[i]);

	if (written)
		*written = n;

	return 0;
}

void tty_receive(struct tty *tty, int c)
{
	spinlock_acquire(&tty->lock);

	switch (c) {
	case CTRL('U'): /* Kill line. */
		while (tty->rx_e != tty->rx_w &&
		       tty->rx_buf[(tty->rx_e - 1) % TTY_RX_BUF_SIZE] != '\n') {
			tty->rx_e--;
			tty_echo(tty, TTY_VIS_BACKSPACE);
		}
		break;
	case CTRL('H'): /* Backspace */
	case '\x7f': /* Delete key */
		if (tty->rx_e != tty->rx_w) {
			tty->rx_e--;
			tty_echo(tty, TTY_VIS_BACKSPACE);
		}
		break;
	case CTRL('P'):
		proc_dump();
		break;
	default:
		if (c != 0 && tty->rx_e - tty->rx_r < TTY_RX_BUF_SIZE) {
			c = (c == '\r') ? '\n' : c;

			tty_echo(tty, c);

			tty->rx_buf[tty->rx_e++ % TTY_RX_BUF_SIZE] = c;

			if (c == '\n' || c == CTRL('D') ||
			    tty->rx_e - tty->rx_r == TTY_RX_BUF_SIZE) {
				tty->rx_w = tty->rx_e;
				proc_wake_all(&tty->rx_r);
			}
		}
		break;
	}

	spinlock_release(&tty->lock);
}

static int tty_chrdev_read(struct file *file, char *buf, usize_t n, usize_t *read)
{
	(void)file;
	if (!tty_chrdev_tty)
		return -ENODEV;
	return tty_read(tty_chrdev_tty, buf, n, read);
}

static int tty_chrdev_write(struct file *file, const char *buf, usize_t n,
			    usize_t *written)
{
	(void)file;
	if (!tty_chrdev_tty)
		return -ENODEV;
	return tty_write(tty_chrdev_tty, buf, n, written);
}

int tty_chrdev_register(struct tty *tty, dev_t dev)
{
	struct chrdev *cd;
	int err;

	if (!tty)
		return -EINVAL;

	cd = chrdev_alloc();
	if (!cd)
		return -ENOMEM;

	cd->ops.read = tty_chrdev_read;
	cd->ops.write = tty_chrdev_write;

	err = chrdev_register(cd, dev);
	if (err) {
		chrdev_free(cd);
		return err;
	}

	tty_chrdev_tty = tty;
	return 0;
}
