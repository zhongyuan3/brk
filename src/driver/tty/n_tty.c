#include <brk/signal.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/termios.h>

void tty_termios_defaults(struct termios *t)
{
	memset(t, 0, sizeof(*t));
	t->c_iflag = ICRNL;
	t->c_oflag = OPOST | ONLCR;
	t->c_cflag = CS8 | CREAD | CLOCAL;
	t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN;

	t->c_cc[VINTR] = 3; /* ^C */
	t->c_cc[VQUIT] = 28; /* ^\ */
	t->c_cc[VERASE] = 127; /* DEL */
	t->c_cc[VKILL] = 21; /* ^U */
	t->c_cc[VEOF] = 4; /* ^D */
	t->c_cc[VMIN] = 1;
	t->c_cc[VTIME] = 0;
	t->c_cc[VSTART] = 17; /* ^Q */
	t->c_cc[VSTOP] = 19; /* ^S */
	t->c_cc[VSUSP] = 26; /* ^Z */
	t->c_cc[VREPRINT] = 18; /* ^R */
	t->c_cc[VWERASE] = 23; /* ^W */
	t->c_cc[VLNEXT] = 22; /* ^V */
}

static void n_tty_put(struct tty *tty, int c)
{
	struct tty_port *port = tty->port;

	if (port && port->driver && port->driver->ops &&
	    port->driver->ops->put_char)
		(void)port->driver->ops->put_char(tty, c);
}

/* Apply output post-processing.  Used by both write() and input echo. */
static void n_tty_output(struct tty *tty, int c)
{
	struct termios *t = &tty->port->termios;

	if ((t->c_oflag & OPOST) && (t->c_oflag & ONLCR) && c == '\n') {
		n_tty_put(tty, '\r');
		n_tty_put(tty, '\n');
	} else {
		n_tty_put(tty, c);
	}
}

static void n_tty_echo_char(struct tty *tty, int c)
{
	struct termios *t = &tty->port->termios;

	if (!(t->c_lflag & ECHO))
		return;

	if ((t->c_lflag & ECHOCTL) && c < 0x20 && c != '\n' && c != '\t' &&
	    c != '\r') {
		n_tty_put(tty, '^');
		n_tty_put(tty, c + 0x40);
		return;
	}

	n_tty_output(tty, c);
}

static void n_tty_erase_echo(struct tty *tty)
{
	n_tty_put(tty, '\b');
	n_tty_put(tty, ' ');
	n_tty_put(tty, '\b');
}

static void n_tty_kill_echo(struct tty *tty, size_t nr)
{
	while (nr-- > 0)
		n_tty_erase_echo(tty);
}

static void n_tty_signal(struct tty *tty, int sig, int c)
{
	struct tty_port *port = tty->port;
	struct termios *t = &port->termios;

	if (!(t->c_lflag & ISIG))
		return;

	if (port->foreground)
		signal_send(port->foreground, sig);

	if (!(t->c_lflag & NOFLSH))
		tty->rx_e = tty->rx_w;

	if (t->c_lflag & ECHO) {
		n_tty_put(tty, '^');
		n_tty_put(tty, c + 0x40);
		n_tty_output(tty, '\n');
	}

	/* Wake a reader blocked in read() so it can observe the signal. */
	task_wake_all(&tty->rx_r);
}

static void n_tty_receive_buf(struct tty *tty, const u8 *buf, size_t size)
{
	struct tty_port *port = tty->port;
	struct termios *t = &port->termios;
	irqstate_t flags = spin_lock_irqsave(&port->lock);

	for (size_t i = 0; i < size; ++i) {
		u8 c = buf[i];

		if ((t->c_iflag & IGNCR) && c == '\r')
			continue;
		if ((t->c_iflag & ICRNL) && c == '\r')
			c = '\n';
		if ((t->c_iflag & INLCR) && c == '\n')
			c = '\r';
		if (c == 0)
			continue;

		if (!(t->c_lflag & ICANON)) {
			if (tty->rx_e - tty->rx_r >= tty->rx_size) {
				tty->rx_w = tty->rx_e;
				task_wake_all(&tty->rx_r);
				continue;
			}
			tty->rx_buf[tty->rx_e % tty->rx_size] = c;
			tty->rx_e++;
			tty->rx_w = tty->rx_e;
			n_tty_echo_char(tty, c);
			task_wake_all(&tty->rx_r);
			continue;
		}

		if (c == t->c_cc[VINTR]) {
			n_tty_signal(tty, SIGINT, c);
			continue;
		}
		if (c == t->c_cc[VQUIT]) {
			n_tty_signal(tty, SIGQUIT, c);
			continue;
		}
		if (c == t->c_cc[VSUSP]) {
			n_tty_signal(tty, SIGTSTP, c);
			continue;
		}

		if (c == t->c_cc[VERASE]) {
			if (tty->rx_e > tty->rx_w) {
				tty->rx_e--;
				n_tty_erase_echo(tty);
			}
			continue;
		}

		if (c == t->c_cc[VKILL]) {
			size_t nr = tty->rx_e - tty->rx_w;

			tty->rx_e = tty->rx_w;
			if (t->c_lflag & ECHOK)
				n_tty_kill_echo(tty, nr);
			continue;
		}

		if (c == t->c_cc[VEOF]) {
			if (tty->rx_e == tty->rx_w) {
				tty->eof_pending = true;
			} else {
				tty->rx_w = tty->rx_e;
			}
			task_wake_all(&tty->rx_r);
			continue;
		}

		if (tty->rx_e - tty->rx_r >= tty->rx_size) {
			/* Publish the full line and let the reader drain it. */
			tty->rx_w = tty->rx_e;
			task_wake_all(&tty->rx_r);
			continue;
		}

		tty->rx_buf[tty->rx_e % tty->rx_size] = c;
		tty->rx_e++;
		n_tty_echo_char(tty, c);

		if (c == '\n' || c == t->c_cc[VEOL] ||
		    c == t->c_cc[VEOL2] ||
		    tty->rx_e - tty->rx_r == tty->rx_size) {
			tty->rx_w = tty->rx_e;
			task_wake_all(&tty->rx_r);
		}
	}

	spin_unlock_irqrestore(&port->lock, flags);
}

static ssize_t n_tty_read(struct tty *tty, char *buf, size_t size)
{
	struct tty_port *port = tty->port;
	char tmp[256];
	size_t total = 0;

	if (size == 0)
		return 0;

	sleeplock_acquire(&tty->read_lock);

	while (total < size) {
		bool canonical = (port->termios.c_lflag & ICANON) != 0;
		size_t avail, chunk;
		irqstate_t flags;

		flags = spin_lock_irqsave(&port->lock);
		while (tty->rx_w == tty->rx_r) {
			if (tty->eof_pending) {
				tty->eof_pending = false;
				spin_unlock_irqrestore(&port->lock, flags);
				sleeplock_release(&tty->read_lock);
				return total > 0 ? (ssize_t)total : 0;
			}
			if (!canonical && port->termios.c_cc[VMIN] == 0) {
				spin_unlock_irqrestore(&port->lock, flags);
				sleeplock_release(&tty->read_lock);
				return (ssize_t)total;
			}
			if (signal_pending(current_task())) {
				spin_unlock_irqrestore(&port->lock, flags);
				sleeplock_release(&tty->read_lock);
				return total > 0 ? (ssize_t)total : -EINTR;
			}
			task_sleep(&tty->rx_r, &port->lock);
			canonical = (port->termios.c_lflag & ICANON) != 0;
		}

		avail = tty->rx_w - tty->rx_r;
		if (canonical) {
			for (size_t i = 0; i < avail; ++i) {
				u8 c = tty->rx_buf[(tty->rx_r + i) %
						   tty->rx_size];
				if (c == '\n') {
					avail = i + 1;
					break;
				}
			}
		}

		chunk = avail;
		if (chunk > size - total)
			chunk = size - total;
		if (chunk > sizeof(tmp))
			chunk = sizeof(tmp);

		for (size_t i = 0; i < chunk; ++i)
			tmp[i] = tty->rx_buf[(tty->rx_r + i) %
					     tty->rx_size];
		tty->rx_r += chunk;

		spin_unlock_irqrestore(&port->lock, flags);

		if (copy_to_user(buf + total, tmp, chunk)) {
			sleeplock_release(&tty->read_lock);
			return total > 0 ? (ssize_t)total : -EFAULT;
		}
		total += chunk;

		/*
		 * Canonical reads return one line at a time.  Raw reads return
		 * whatever is available instead of waiting for the full request.
		 */
		break;
	}

	sleeplock_release(&tty->read_lock);
	return (ssize_t)total;
}

static ssize_t n_tty_write(struct tty *tty, const char *buf, size_t size)
{
	struct tty_port *port = tty->port;
	char tmp[128];
	size_t total = 0;
	int err = 0;

	sleeplock_acquire(&tty->write_lock);

	while (total < size) {
		size_t chunk = size - total;
		size_t i;

		if (chunk > sizeof(tmp))
			chunk = sizeof(tmp);
		if (copy_from_user(tmp, buf + total, chunk)) {
			if (total == 0)
				total = (size_t)-EFAULT;
			break;
		}

		irqstate_t flags = spin_lock_irqsave(&port->lock);
		for (i = 0; i < chunk; ++i) {
			int c = tmp[i];

			if ((port->termios.c_oflag & OPOST) &&
			    (port->termios.c_oflag & ONLCR) && c == '\n') {
				err = port->driver->ops->put_char(tty, '\r');
				if (!err)
					err = port->driver->ops->put_char(tty,
									  '\n');
			} else {
				err = port->driver->ops->put_char(tty, c);
			}
			if (err)
				break;
		}
		spin_unlock_irqrestore(&port->lock, flags);

		total += i;
		if (err)
			break;
	}

	sleeplock_release(&tty->write_lock);

	if (total > 0 && (ssize_t)total > 0)
		return (ssize_t)total;
	if (total == (size_t)-EFAULT)
		return -EFAULT;
	return err;
}

static long n_tty_ioctl(struct tty *tty, unsigned int cmd, unsigned long arg)
{
	struct tty_port *port = tty->port;
	struct termios t;
	int avail;
	irqstate_t flags;

	if (!arg && (cmd == TCGETS || cmd == TCSETS || cmd == TCSETSW ||
		     cmd == TCSETSF || cmd == FIONREAD))
		return -EFAULT;

	switch (cmd) {
	case TCGETS:
		flags = spin_lock_irqsave(&port->lock);
		t = port->termios;
		spin_unlock_irqrestore(&port->lock, flags);
		return copy_to_user((void *)arg, &t, sizeof(t)) ? -EFAULT : 0;

	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		if (copy_from_user(&t, (const void *)arg, sizeof(t)))
			return -EFAULT;

		/* This driver only supports 8-bit, receiver-enabled lines. */
		t.c_cflag = (t.c_cflag & ~CSIZE) | CS8;
		t.c_cflag |= CREAD | CLOCAL;

		flags = spin_lock_irqsave(&port->lock);
		port->termios = t;
		if (cmd == TCSETSF) {
			tty->rx_r = 0;
			tty->rx_w = 0;
			tty->rx_e = 0;
			tty->eof_pending = false;
		}
		spin_unlock_irqrestore(&port->lock, flags);
		task_wake_all(&tty->rx_r);
		return 0;

	case TCFLSH:
		flags = spin_lock_irqsave(&port->lock);
		if (arg == TCIFLUSH || arg == TCIOFLUSH) {
			tty->rx_r = 0;
			tty->rx_w = 0;
			tty->rx_e = 0;
			tty->eof_pending = false;
		}
		spin_unlock_irqrestore(&port->lock, flags);
		return 0;

	case FIONREAD:
		flags = spin_lock_irqsave(&port->lock);
		avail = (int)(tty->rx_w - tty->rx_r);
		spin_unlock_irqrestore(&port->lock, flags);
		if (avail < 0)
			avail = 0;
		return copy_to_user((void *)arg, &avail, sizeof(avail)) ?
			       -EFAULT :
			       0;

	default:
		return -ENOTTY;
	}
}

static int n_tty_open(struct tty *tty)
{
	(void)tty;
	return 0;
}

static void n_tty_close(struct tty *tty)
{
	(void)tty;
}

const struct tty_ldisc_ops n_tty_ldisc_ops = {
	.name = "n_tty",
	.open = n_tty_open,
	.close = n_tty_close,
	.read = n_tty_read,
	.write = n_tty_write,
	.receive_buf = n_tty_receive_buf,
	.ioctl = n_tty_ioctl,
};
