#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/list.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/termios.h>
#include <brk/timer.h>
#include <brk/tty.h>
#include <brk/uart.h>

#define TTY_VIS_BACKSPACE 0x100
#define CTRL(x) ((x) - '@')

_Static_assert(sizeof(struct termios) == 36,
	       "struct termios must match Linux uapi for TCGETS/TCSETS");
_Static_assert(sizeof(struct winsize) == 8,
	       "struct winsize must match Linux uapi for TIOCGWINSZ");

static void uart_boot_put_char(struct tty_port *port, int c)
{
	(void)port;

	if (c == TTY_VIS_BACKSPACE) {
		uart_putc('\b');
		uart_putc(' ');
		uart_putc('\b');
	} else {
		uart_putc(c);
	}
}

static struct tty_port uart_boot_port = {
	.put_char = uart_boot_put_char,
};

static struct tty boot_tty;

static void tty_flush_input(struct tty *tty)
{
	tty->rx_r = 0;
	tty->rx_w = 0;
	tty->rx_e = 0;
}

void tty_boot_init(void)
{
	tty_init(&boot_tty, &uart_boot_port);
}

struct tty *tty_boot(void)
{
	return &boot_tty;
}

struct tty_file_priv *tty_file_priv_create(void)
{
	struct tty_file_priv *p;

	p = kmalloc(sizeof(*p));
	if (!p)
		return NULL;
	p->raw_deadline_jiffies = 0;
	p->raw_timer_armed = false;
	list_init(&p->vtime_link);
	return p;
}

void tty_file_priv_destroy(struct tty_file_priv *priv)
{
	if (!priv)
		return;
	list_del_init(&priv->vtime_link);
	kfree(priv);
}

void tty_init(struct tty *tty, struct tty_port *port)
{
	spinlock_init(&tty->lock, "tty");
	tty->port = port;
	memset(&tty->termios, 0, sizeof(tty->termios));
	tty->termios.c_cflag = CS8 | CREAD;
	tty->termios.c_lflag = ICANON | ECHO | ECHOE | ISIG;
	tty->termios.c_cc[VMIN] = 1;
	tty->termios.c_cc[VTIME] = 0;
	tty->termios.c_cc[VERASE] = 0x7f;
	tty->termios.c_cc[VINTR] = CTRL('C');
	tty->winsize.ws_row = 24;
	tty->winsize.ws_col = 80;
	tty->winsize.ws_xpixel = 0;
	tty->winsize.ws_ypixel = 0;
	list_init(&tty->vtime_waiters);
	tty->null_raw_armed = false;
	tty->null_raw_deadline_jiffies = 0;
	tty_flush_input(tty);
}

void tty_timer_tick(void)
{
	struct tty *tty = tty_boot();
	bool wake = false;

	spinlock_acquire(&tty->lock);
	if (tty->null_raw_armed &&
	    (s64)(jiffies_get() - tty->null_raw_deadline_jiffies) >= 0)
		wake = true;
	if (!wake) {
		struct tty_file_priv *p;

		list_for_each_entry(p, &tty->vtime_waiters, vtime_link) {
			if (p->raw_timer_armed &&
			    (s64)(jiffies_get() - p->raw_deadline_jiffies) >=
				    0) {
				wake = true;
				break;
			}
		}
	}
	spinlock_release(&tty->lock);

	if (wake)
		proc_wake_all(&tty->rx_r);
}

static void tty_echo(struct tty *tty, int c)
{
	if (!(tty->termios.c_lflag & ECHO))
		return;
	if (!tty->port || !tty->port->put_char)
		return;

	if (c == TTY_VIS_BACKSPACE) {
		tty->port->put_char(tty->port, TTY_VIS_BACKSPACE);
		return;
	}

	tty->port->put_char(tty->port, c);
}

static usize_t tty_rx_avail(struct tty *tty)
{
	return tty->rx_w - tty->rx_r;
}

static int tty_read_noncanon(struct tty *tty, struct tty_file_priv *priv,
			     char *buf, usize_t n, usize_t *read_out)
{
	unsigned char vmin = tty->termios.c_cc[VMIN];
	unsigned char vtime = tty->termios.c_cc[VTIME];
	usize_t avail;
	usize_t give;

	if (vmin == 0 && vtime == 0) {
		avail = tty_rx_avail(tty);
		if (avail == 0) {
			while (tty_rx_avail(tty) == 0) {
				if (proc_is_killed(current_process()))
					return -1;
				proc_sleep(&tty->rx_r, &tty->lock);
			}
			avail = tty_rx_avail(tty);
		}
		give = avail < n ? avail : n;
		for (usize_t i = 0; i < give; ++i)
			*buf++ = tty->rx_buf[tty->rx_r++ % TTY_RX_BUF_SIZE];
		*read_out = give;
		return 0;
	}

	if (vmin == 0 && vtime > 0) {
		avail = tty_rx_avail(tty);
		if (avail > 0) {
			give = avail < n ? avail : n;
			for (usize_t i = 0; i < give; ++i)
				*buf++ = tty->rx_buf[tty->rx_r++ %
						     TTY_RX_BUF_SIZE];
			*read_out = give;
			return 0;
		}

		if (priv) {
			list_add_tail(&priv->vtime_link, &tty->vtime_waiters);
			priv->raw_deadline_jiffies =
				jiffies_get() + (u64)vtime * 100u;
			priv->raw_timer_armed = true;
		} else {
			tty->null_raw_deadline_jiffies =
				jiffies_get() + (u64)vtime * 100u;
			tty->null_raw_armed = true;
		}

		while (tty_rx_avail(tty) == 0) {
			if (proc_is_killed(current_process())) {
				if (priv) {
					priv->raw_timer_armed = false;
					list_del_init(&priv->vtime_link);
				} else
					tty->null_raw_armed = false;
				return -1;
			}
			if (priv) {
				if ((s64)(jiffies_get() -
					  priv->raw_deadline_jiffies) >= 0) {
					priv->raw_timer_armed = false;
					list_del_init(&priv->vtime_link);
					*read_out = 0;
					return 0;
				}
			} else {
				if ((s64)(jiffies_get() -
					  tty->null_raw_deadline_jiffies) >=
				    0) {
					tty->null_raw_armed = false;
					*read_out = 0;
					return 0;
				}
			}
			proc_sleep(&tty->rx_r, &tty->lock);
		}

		if (priv) {
			priv->raw_timer_armed = false;
			list_del_init(&priv->vtime_link);
		} else
			tty->null_raw_armed = false;

		avail = tty_rx_avail(tty);
		give = avail < n ? avail : n;
		for (usize_t i = 0; i < give; ++i)
			*buf++ = tty->rx_buf[tty->rx_r++ % TTY_RX_BUF_SIZE];
		*read_out = give;
		return 0;
	}

	usize_t need = vmin;

	if (need == 0)
		need = 1;
	while (tty_rx_avail(tty) < need) {
		if (proc_is_killed(current_process()))
			return -1;
		proc_sleep(&tty->rx_r, &tty->lock);
	}
	avail = tty_rx_avail(tty);
	give = avail < n ? avail : n;
	for (usize_t i = 0; i < give; ++i)
		*buf++ = tty->rx_buf[tty->rx_r++ % TTY_RX_BUF_SIZE];
	*read_out = give;
	return 0;
}

int tty_read(struct tty *tty, struct file *file, char *buf, usize_t n,
	     usize_t *read)
{
	usize_t target = n;
	int c;
	struct tty_file_priv *priv = file ? file->private_data : NULL;

	spinlock_acquire(&tty->lock);
	if (tty->termios.c_lflag & ICANON) {
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

	usize_t rc = 0;
	int err = tty_read_noncanon(tty, priv, buf, n, &rc);

	spinlock_release(&tty->lock);
	if (read)
		*read = rc;
	return err;
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

	if (!(tty->termios.c_lflag & ICANON)) {
		if (c == 0) {
			spinlock_release(&tty->lock);
			return;
		}
		if (tty->rx_w - tty->rx_r >= TTY_RX_BUF_SIZE) {
			spinlock_release(&tty->lock);
			return;
		}
		tty_echo(tty, c);
		tty->rx_buf[tty->rx_w++ % TTY_RX_BUF_SIZE] = (char)c;
		tty->rx_e = tty->rx_w;
		proc_wake_all(&tty->rx_r);
		spinlock_release(&tty->lock);
		return;
	}

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

static void tty_termios_set(struct tty *tty, const struct termios *src,
			    int flush_rx)
{
	tcflag_t old = tty->termios.c_lflag;

	if (flush_rx)
		tty_flush_input(tty);
	memcpy(&tty->termios, src, sizeof(tty->termios));
	if (!flush_rx && ((old ^ tty->termios.c_lflag) & ICANON))
		tty_flush_input(tty);
}

long tty_ioctl(struct tty *tty, struct file *file, unsigned int cmd,
	       unsigned long arg)
{
	(void)file;

	switch (cmd) {
	case TCGETS: {
		struct termios *p = (struct termios *)arg;

		if (!p)
			return -EINVAL;
		spinlock_acquire(&tty->lock);
		memcpy(p, &tty->termios, sizeof(*p));
		spinlock_release(&tty->lock);
		return 0;
	}
	case TCSETS:
	case TCSETSW:
		if (!arg)
			return -EINVAL;
		spinlock_acquire(&tty->lock);
		tty_termios_set(tty, (const struct termios *)arg, 0);
		spinlock_release(&tty->lock);
		return 0;
	case TCSETSF:
		if (!arg)
			return -EINVAL;
		spinlock_acquire(&tty->lock);
		tty_termios_set(tty, (const struct termios *)arg, 1);
		spinlock_release(&tty->lock);
		return 0;
	case TIOCGWINSZ: {
		struct winsize *wp = (struct winsize *)arg;

		if (!wp)
			return -EINVAL;
		spinlock_acquire(&tty->lock);
		memcpy(wp, &tty->winsize, sizeof(*wp));
		spinlock_release(&tty->lock);
		return 0;
	}
	case TIOCSWINSZ: {
		struct winsize *wp = (struct winsize *)arg;
		struct winsize tmp;

		if (!wp)
			return -EINVAL;
		memcpy(&tmp, wp, sizeof(tmp));
		spinlock_acquire(&tty->lock);
		tty->winsize = tmp;
		spinlock_release(&tty->lock);
		return 0;
	}
	case BRK_TIOCGLFLAGS: {
		u32 *p = (u32 *)arg;

		if (!p)
			return -EINVAL;
		spinlock_acquire(&tty->lock);
		*p = tty->termios.c_lflag;
		spinlock_release(&tty->lock);
		return 0;
	}
	case BRK_TIOCSLFLAGS: {
		u32 *p = (u32 *)arg;
		tcflag_t old;

		if (!p)
			return -EINVAL;
		spinlock_acquire(&tty->lock);
		old = tty->termios.c_lflag;
		tty->termios.c_lflag =
			(tty->termios.c_lflag & ~(ICANON | ECHO)) |
			(*p & (ICANON | ECHO));
		if ((old ^ tty->termios.c_lflag) & ICANON)
			tty_flush_input(tty);
		spinlock_release(&tty->lock);
		return 0;
	}
	case BRK_TIOCGETATTR: {
		struct brk_tty_attr *p = (struct brk_tty_attr *)arg;

		if (!p)
			return -EINVAL;
		spinlock_acquire(&tty->lock);
		p->lflags = tty->termios.c_lflag;
		p->vmin = tty->termios.c_cc[VMIN];
		p->vtime = tty->termios.c_cc[VTIME];
		p->_pad[0] = 0;
		p->_pad[1] = 0;
		spinlock_release(&tty->lock);
		return 0;
	}
	case BRK_TIOCSETATTR: {
		struct brk_tty_attr *p = (struct brk_tty_attr *)arg;
		struct brk_tty_attr tmp;
		tcflag_t old;

		if (!p)
			return -EINVAL;
		memcpy(&tmp, p, sizeof(tmp));
		spinlock_acquire(&tty->lock);
		old = tty->termios.c_lflag;
		tty->termios.c_lflag =
			(tty->termios.c_lflag & ~(ICANON | ECHO)) |
			(tmp.lflags & (ICANON | ECHO));
		tty->termios.c_cc[VMIN] = tmp.vmin;
		tty->termios.c_cc[VTIME] = tmp.vtime;
		if ((old ^ tty->termios.c_lflag) & ICANON)
			tty_flush_input(tty);
		spinlock_release(&tty->lock);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}
