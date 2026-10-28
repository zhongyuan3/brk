#include <brk/ktime.h>
#include <brk/signal.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/termios.h>

#define VTIME_UNIT_NS 100000000ULL /* 0.1 second */

void tty_termios_defaults(struct termios *t)
{
	memset(t, 0, sizeof(*t));
	t->c_iflag = ICRNL | IXON;
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
	t->c_cc[VDISCARD] = 15; /* ^O */
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

	if (t->c_lflag & FLUSHO)
		return;

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

static bool n_tty_background(struct tty_port *port, struct task *task)
{
	return port->pgrp > 0 && task->pgid != port->pgrp;
}

static void n_tty_signal(struct tty *tty, int sig, int c)
{
	struct tty_port *port = tty->port;
	struct termios *t = &port->termios;

	if (!(t->c_lflag & ISIG))
		return;

	if (port->pgrp > 0)
		signal_send_pgrp(port->pgrp, sig);

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

/* Store one byte without interpreting it as an editing character. */
static void n_tty_store(struct tty *tty, u8 c)
{
	struct termios *t = &tty->port->termios;
	bool canonical = (t->c_lflag & ICANON) != 0;

	if (tty->rx_e - tty->rx_r >= tty->rx_size) {
		/* Publish the buffer and let the reader drain it. */
		tty->rx_w = tty->rx_e;
		task_wake_all(&tty->rx_r);
		return;
	}

	tty->rx_buf[tty->rx_e % tty->rx_size] = c;
	tty->rx_e++;
	n_tty_echo_char(tty, c);

	if (!canonical) {
		tty->rx_w = tty->rx_e;
		task_wake_all(&tty->rx_r);
	}
}

static void n_tty_werase(struct tty *tty)
{
	struct termios *t = &tty->port->termios;
	size_t start = tty->rx_w;
	size_t end = tty->rx_e;

	/* Skip trailing blanks, then remove the previous word. */
	while (end > start) {
		u8 c = tty->rx_buf[(end - 1) % tty->rx_size];

		if (c != ' ' && c != '\t')
			break;
		end--;
	}
	while (end > start) {
		u8 c = tty->rx_buf[(end - 1) % tty->rx_size];

		if (c == ' ' || c == '\t')
			break;
		end--;
	}

	if (t->c_lflag & ECHOE)
		n_tty_kill_echo(tty, tty->rx_e - end);
	tty->rx_e = end;
}

static void n_tty_reprint(struct tty *tty)
{
	struct termios *t = &tty->port->termios;

	if (!(t->c_lflag & ECHO))
		return;

	n_tty_put(tty, '^');
	n_tty_put(tty, 'R');
	n_tty_output(tty, '\n');

	for (size_t i = tty->rx_w; i < tty->rx_e; ++i)
		n_tty_echo_char(tty,
				tty->rx_buf[i % tty->rx_size]);
	n_tty_output(tty, '\n');
}

static void n_tty_receive_buf(struct tty *tty, const u8 *buf, size_t size)
{
	struct tty_port *port = tty->port;
	struct termios *t = &port->termios;
	irqstate_t flags = spin_lock_irqsave(&port->lock);

	for (size_t i = 0; i < size; ++i) {
		u8 c = buf[i];
		bool canonical = (t->c_lflag & ICANON) != 0;

		if ((t->c_iflag & IGNCR) && c == '\r')
			continue;
		if ((t->c_iflag & ICRNL) && c == '\r')
			c = '\n';
		if ((t->c_iflag & INLCR) && c == '\n')
			c = '\r';
		if (c == 0)
			continue;

		/* VLNEXT: the next character is taken literally. */
		if (tty->lnext) {
			tty->lnext = false;
			n_tty_store(tty, c);
			continue;
		}

		if ((t->c_lflag & IEXTEN) && c == t->c_cc[VLNEXT]) {
			tty->lnext = true;
			if (t->c_lflag & ECHO) {
				n_tty_put(tty, '^');
				n_tty_put(tty, '\b');
			}
			continue;
		}

		/* IXON output flow control. */
		if (t->c_iflag & IXON) {
			if (c == t->c_cc[VSTOP]) {
				tty->stopped = true;
				continue;
			}
			if (c == t->c_cc[VSTART]) {
				tty->stopped = false;
				task_wake_all(&tty->stopped);
				continue;
			}
		}

		/* VDISCARD toggles output discard. */
		if (canonical && (t->c_lflag & IEXTEN) &&
		    c == t->c_cc[VDISCARD]) {
			t->c_lflag ^= FLUSHO;
			continue;
		}

		if (t->c_lflag & ISIG) {
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
		}

		if (!canonical) {
			n_tty_store(tty, c);
			continue;
		}

		if (c == t->c_cc[VERASE]) {
			if (tty->rx_e > tty->rx_w) {
				tty->rx_e--;
				if (t->c_lflag & ECHOE)
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

		if ((t->c_lflag & IEXTEN) && c == t->c_cc[VWERASE]) {
			n_tty_werase(tty);
			continue;
		}

		if ((t->c_lflag & IEXTEN) && c == t->c_cc[VREPRINT]) {
			n_tty_reprint(tty);
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

		n_tty_store(tty, c);

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
	struct task *task = current_task();
	char tmp[256];
	size_t total = 0;
	size_t seen = 0;
	u64 deadline = 0;
	bool timed = false;

	if (size == 0)
		return 0;

	sleeplock_acquire(&tty->read_lock);

	while (total < size) {
		bool canonical;
		size_t avail, min, chunk, copy, i;
		unsigned int vtime;
		irqstate_t flags;

		flags = spin_lock_irqsave(&port->lock);

		/* Job control: background reads get SIGTTIN. */
		if (n_tty_background(port, task) &&
		    (port->termios.c_lflag & ISIG)) {
			signal_send_pgrp(task->pgid, SIGTTIN);
			spin_unlock_irqrestore(&port->lock, flags);
			sleeplock_release(&tty->read_lock);
			return total > 0 ? (ssize_t)total : -EINTR;
		}

		canonical = (port->termios.c_lflag & ICANON) != 0;
		min = port->termios.c_cc[VMIN];
		vtime = canonical ? 0 : port->termios.c_cc[VTIME];

		avail = tty->rx_w - tty->rx_r;
		copy = 0;

		if (canonical) {
			if (avail > 0) {
				bool found = false;
				size_t line = 0;

				for (i = 0; i < avail; ++i) {
					if (tty->rx_buf[(tty->rx_r + i) %
							tty->rx_size] ==
					    '\n') {
						line = i + 1;
						found = true;
						break;
					}
				}
				if (found)
					copy = line;
				else if (avail >= tty->rx_size)
					copy = avail;
			}

			if (copy == 0) {
				if (tty->eof_pending && total == 0) {
					tty->eof_pending = false;
					spin_unlock_irqrestore(&port->lock,
							       flags);
					sleeplock_release(&tty->read_lock);
					return 0;
				}
				if (signal_pending(task)) {
					spin_unlock_irqrestore(&port->lock,
							       flags);
					sleeplock_release(&tty->read_lock);
					return total > 0 ? (ssize_t)total :
							   -EINTR;
				}
				task_sleep(&tty->rx_r, &port->lock);
				spin_unlock_irqrestore(&port->lock, flags);
				continue;
			}
		} else {
			if (min == 0)
				copy = avail;
			else
				copy = avail >= min ? avail : 0;

			if (copy == 0) {
				u64 now;

				if (signal_pending(task)) {
					spin_unlock_irqrestore(&port->lock,
							       flags);
					sleeplock_release(&tty->read_lock);
					return total > 0 ? (ssize_t)total :
							   -EINTR;
				}

				/* MIN=0, TIME=0: non-blocking. */
				if (min == 0 && vtime == 0) {
					spin_unlock_irqrestore(&port->lock,
							       flags);
					sleeplock_release(&tty->read_lock);
					return (ssize_t)total;
				}

				now = ktime_get_boot_ns();
				if (vtime > 0) {
					if (min == 0 && !timed) {
						timed = true;
						deadline = now +
							(u64)vtime *
							VTIME_UNIT_NS;
					} else if (min > 0 && avail > 0 &&
						   avail > seen) {
						seen = avail;
						timed = true;
						deadline = now +
							(u64)vtime *
							VTIME_UNIT_NS;
					}

					if (timed && now >= deadline) {
						if (avail > 0)
							copy = avail;
						else {
							spin_unlock_irqrestore(
								&port->lock,
								flags);
							sleeplock_release(
								&tty->read_lock);
							return (ssize_t)total;
						}
					} else {
						task_sleep(
							timekeeper_wait_chan(),
							&port->lock);
						spin_unlock_irqrestore(
							&port->lock, flags);
						continue;
					}
				} else {
					task_sleep(&tty->rx_r, &port->lock);
					spin_unlock_irqrestore(&port->lock,
							       flags);
					continue;
				}
			}
		}

		if (copy == 0) {
			spin_unlock_irqrestore(&port->lock, flags);
			sleeplock_release(&tty->read_lock);
			return (ssize_t)total;
		}

		chunk = copy;
		if (chunk > size - total)
			chunk = size - total;
		if (chunk > sizeof(tmp))
			chunk = sizeof(tmp);

		for (i = 0; i < chunk; ++i)
			tmp[i] = tty->rx_buf[(tty->rx_r + i) %
					     tty->rx_size];
		tty->rx_r += chunk;

		spin_unlock_irqrestore(&port->lock, flags);

		if (copy_to_user(buf + total, tmp, chunk)) {
			sleeplock_release(&tty->read_lock);
			return total > 0 ? (ssize_t)total : -EFAULT;
		}
		total += chunk;

		/* Return at most one line/chunk per read, like the old path. */
		break;
	}

	sleeplock_release(&tty->read_lock);
	return (ssize_t)total;
}

static ssize_t n_tty_write(struct tty *tty, const char *buf, size_t size)
{
	struct tty_port *port = tty->port;
	struct task *task = current_task();
	char tmp[128];
	size_t total = 0;
	int err = 0;

	sleeplock_acquire(&tty->write_lock);

	/* TOSTOP: background writers get SIGTTOU. */
	{
		irqstate_t flags = spin_lock_irqsave(&port->lock);
		bool bg = n_tty_background(port, task) &&
			  (port->termios.c_lflag & TOSTOP);

		if (bg) {
			signal_send_pgrp(task->pgid, SIGTTOU);
			spin_unlock_irqrestore(&port->lock, flags);
			sleeplock_release(&tty->write_lock);
			return -EINTR;
		}
		spin_unlock_irqrestore(&port->lock, flags);
	}

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

		/* IXON output flow control. */
		for (;;) {
			irqstate_t flags = spin_lock_irqsave(&port->lock);
			bool stopped = tty->stopped &&
				       (port->termios.c_iflag & IXON);

			if (!stopped) {
				spin_unlock_irqrestore(&port->lock, flags);
				break;
			}
			task_sleep(&tty->stopped, &port->lock);
			spin_unlock_irqrestore(&port->lock, flags);
		}

		irqstate_t flags = spin_lock_irqsave(&port->lock);
		for (i = 0; i < chunk; ++i) {
			int c = tmp[i];
			struct termios *t = &port->termios;

			if (t->c_lflag & FLUSHO)
				continue;

			if ((t->c_oflag & OPOST) &&
			    (t->c_oflag & ONLCR) && c == '\n') {
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
			tty->lnext = false;
		}
		spin_unlock_irqrestore(&port->lock, flags);
		task_wake_all(&tty->rx_r);
		task_wake_all(&tty->stopped);
		return 0;

	case TCFLSH:
		flags = spin_lock_irqsave(&port->lock);
		if (arg == TCIFLUSH || arg == TCIOFLUSH) {
			tty->rx_r = 0;
			tty->rx_w = 0;
			tty->rx_e = 0;
			tty->eof_pending = false;
			tty->lnext = false;
		}
		spin_unlock_irqrestore(&port->lock, flags);
		task_wake_all(&tty->rx_r);
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
