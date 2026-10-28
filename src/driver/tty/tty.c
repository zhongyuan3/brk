#include <brk/kmalloc.h>
#include <brk/refcnt.h>
#include <brk/signal.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/ioctl.h>
#include <uapi/signal.h>
#include <uapi/types.h>

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
	tty->ldisc = &n_tty_ldisc_ops;
	tty->eof_pending = false;
	/* The port owns the initial reference. */
	refcnt_init(&tty->refcnt, 1);
	sleeplock_init(&tty->read_lock, "tty.read_lock");
	sleeplock_init(&tty->write_lock, "tty.write_lock");
	return tty;
}

void tty_free(struct tty *tty)
{
	kfree(tty->rx_buf);
	kfree(tty);
}

static void tty_put(struct tty *tty)
{
	if (!tty)
		return;
	if (refcnt_dec_fetch(&tty->refcnt) > 0)
		return;
	tty_free(tty);
}

void tty_init(struct tty *tty, struct tty_port *port)
{
	tty->port = port;
	tty_init_winsize(tty);
}

ssize_t tty_read(struct tty *tty, char *buf, size_t n)
{
	if (!tty || tty->dead || !tty->ldisc || !tty->ldisc->read)
		return -EIO;

	return tty->ldisc->read(tty, buf, n);
}

ssize_t tty_write(struct tty *tty, const void *buf, size_t n)
{
	if (!tty || tty->dead || !tty->ldisc || !tty->ldisc->write)
		return -EIO;
	return tty->ldisc->write(tty, buf, n);
}

void tty_receive(struct tty *tty, int c)
{
	u8 ch = (u8)c;

	if (!tty || tty->dead || !tty->ldisc || !tty->ldisc->receive_buf)
		return;
	tty->ldisc->receive_buf(tty, &ch, 1);
}

long tty_ioctl(struct tty *tty, unsigned int cmd, unsigned long arg)
{
	struct tty_port *port;
	struct winsize ws;
	struct task *task;
	pid_t pgrp;
	irqstate_t flags;
	long ret;

	if (!tty || !tty->port || tty->dead)
		return -EIO;

	port = tty->port;

	switch (cmd) {
	case TIOCGWINSZ:
		if (!arg)
			return -EFAULT;

		flags = spin_lock_irqsave(&port->lock);
		ws = tty->winsize;
		spin_unlock_irqrestore(&port->lock, flags);

		return copy_to_user((void *)arg, &ws, sizeof(ws)) ? -EFAULT : 0;

	case TIOCSWINSZ:
		if (!arg || copy_from_user(&ws, (const void *)arg, sizeof(ws)))
			return -EFAULT;

		flags = spin_lock_irqsave(&port->lock);
		tty->winsize = ws;
		pgrp = port->pgrp;
		spin_unlock_irqrestore(&port->lock, flags);

		if (pgrp > 0)
			signal_send_pgrp(pgrp, SIGWINCH);
		return 0;

	case TIOCGPGRP:
		if (!arg)
			return -EFAULT;

		flags = spin_lock_irqsave(&port->lock);
		pgrp = port->pgrp;
		spin_unlock_irqrestore(&port->lock, flags);

		return copy_to_user((void *)arg, &pgrp, sizeof(pgrp)) ?
			       -EFAULT :
			       0;

	case TIOCSPGRP:
		if (!arg || copy_from_user(&pgrp, (const void *)arg,
					   sizeof(pgrp)))
			return -EFAULT;
		if (pgrp <= 0)
			return -EINVAL;

		task = current_task();
		flags = spin_lock_irqsave(&port->lock);
		if (port->sid && port->sid != task->sid) {
			spin_unlock_irqrestore(&port->lock, flags);
			return -ENOTTY;
		}
		spin_unlock_irqrestore(&port->lock, flags);

		if (!task_pgrp_exists(pgrp, task->sid))
			return -EPERM;

		flags = spin_lock_irqsave(&port->lock);
		port->pgrp = pgrp;
		spin_unlock_irqrestore(&port->lock, flags);
		task_wake_all(&port->pgrp);
		return 0;

	case TIOCSCTTY:
		task = current_task();
		if (task->sid != task->tgid)
			return -EPERM;

		flags = spin_lock_irqsave(&port->lock);
		if (port->sid && port->sid != task->sid) {
			spin_unlock_irqrestore(&port->lock, flags);
			return -EPERM;
		}
		port->sid = task->sid;
		port->pgrp = task->pgid;
		spin_unlock_irqrestore(&port->lock, flags);
		task_wake_all(&port->pgrp);
		return 0;

	case TIOCNOTTY:
		task = current_task();
		flags = spin_lock_irqsave(&port->lock);
		if (!port->sid || port->sid != task->sid) {
			spin_unlock_irqrestore(&port->lock, flags);
			return -ENOTTY;
		}
		pgrp = port->pgrp;
		port->sid = 0;
		port->pgrp = 0;
		spin_unlock_irqrestore(&port->lock, flags);
		task_wake_all(&port->pgrp);

		if (pgrp > 0) {
			signal_send_pgrp(pgrp, SIGHUP);
			signal_send_pgrp(pgrp, SIGCONT);
		}
		return 0;

	default:
		break;
	}

	if (tty->ldisc && tty->ldisc->ioctl) {
		ret = tty->ldisc->ioctl(tty, cmd, arg);
		if (ret != -ENOTTY)
			return ret;
	}

	return -ENOTTY;
}

struct tty *tty_attach_port(struct tty_port *port)
{
	struct tty *tty;

	if (!port)
		return NULL;

	irqstate_t flags = spin_lock_irqsave(&port->lock);

	if (port->tty) {
		spin_unlock_irqrestore(&port->lock, flags);
		return port->tty;
	}

	tty = tty_alloc();
	if (!tty) {
		spin_unlock_irqrestore(&port->lock, flags);
		return NULL;
	}

	tty_init(tty, port);
	port->tty = tty;
	spin_unlock_irqrestore(&port->lock, flags);
	return tty;
}

void tty_detach_port(struct tty_port *port)
{
	struct tty *tty;

	if (!port)
		return;

	irqstate_t flags = spin_lock_irqsave(&port->lock);

	tty = port->tty;
	port->tty = NULL;
	port->dead = true;
	if (tty)
		tty->dead = true;
	spin_unlock_irqrestore(&port->lock, flags);

	/* Wake readers/writers waiting on this port. */
	task_wake_all(&port->pgrp);
	if (tty) {
		task_wake_all(&tty->rx_r);
		task_wake_all(&tty->stopped);
	}

	/* Drop the port's reference; open files keep their own. */
	tty_put(tty);
}

struct tty *tty_open(struct tty_port *port)
{
	struct tty *tty;
	struct task *task = current_task();

	if (!port)
		return NULL;

	irqstate_t flags = spin_lock_irqsave(&port->lock);

	tty = port->tty;
	if (!tty || port->dead || tty->dead) {
		spin_unlock_irqrestore(&port->lock, flags);
		return NULL;
	}

	/*
	 * The first session leader to open an unassociated terminal makes it
	 * the controlling terminal.  Otherwise just remember the foreground
	 * group if none was set yet.
	 */
	if (!port->sid && task->sid == task->tgid) {
		port->sid = task->sid;
		port->pgrp = task->pgid;
	} else if (!port->pgrp) {
		port->pgrp = task->pgid;
	}

	refcnt_inc(&tty->refcnt);
	port->open_count++;
	spin_unlock_irqrestore(&port->lock, flags);
	return tty;
}

void tty_close(struct tty *tty)
{
	struct tty_port *port;
	irqstate_t flags;

	if (!tty)
		return;

	port = tty->port;
	if (port) {
		flags = spin_lock_irqsave(&port->lock);
		if (port->open_count > 0)
			port->open_count--;
		spin_unlock_irqrestore(&port->lock, flags);
	}

	tty_put(tty);
}
