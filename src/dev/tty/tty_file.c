#include <brk/fs.h>
#include <brk/spinlock.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/poll.h>

#include "tty_internal.h"

static int tty_file_open(struct fs_inode *inode, struct fs_file *file)
{
	/*
	 * The file keeps chrdev_fops as its dispatch table so that the
	 * character-device core can hold a cdev reference for the whole
	 * open lifetime.  tty_fops is reached through cdev->fops.
	 */
	file->private_data = NULL;

	struct tty_port *port = tty_lookup_port(inode->rdev);
	if (!port)
		return -ENXIO;

	struct tty *tty = tty_open(port);
	if (!tty)
		return -ENOMEM;

	file->private_data = tty;
	return 0;
}

static int tty_file_release(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;

	struct tty *tty = file->private_data;
	if (!tty)
		return -EINVAL;

	tty_close(tty);
	return 0;
}

static ssize_t tty_file_read(struct fs_file *file, char *buf, size_t size,
			     loff_t *pos)
{
	(void)pos;
	struct tty *tty = file->private_data;
	if (!tty)
		return -EINVAL;
	return tty_read(tty, buf, size);
}

static ssize_t tty_file_write(struct fs_file *file, const char *buf,
			      size_t size, loff_t *pos)
{
	(void)pos;
	struct tty *tty = file->private_data;
	if (!tty)
		return -EINVAL;
	return tty_write(tty, buf, size);
}

static loff_t tty_file_llseek(struct fs_file *file, loff_t offset, int whence)
{
	(void)whence;
	(void)offset;
	(void)file;
	return 0;
}

static int tty_file_iterate_shared(struct fs_file *file,
				   struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int tty_file_sync(struct fs_file *file, loff_t start, loff_t end,
			 int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int tty_file_flush(struct fs_file *file)
{
	(void)file;
	return 0;
}

static long tty_file_ioctl(struct fs_file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct tty *tty = file->private_data;

	return tty_ioctl(tty, cmd, arg);
}

static unsigned int tty_file_poll(struct fs_file *file, unsigned int events)
{
	struct tty *tty = file->private_data;
	struct tty_port *port;
	unsigned int rev = 0;
	irqstate_t flags;

	if (!tty || !tty->port || tty->dead)
		return POLLERR | POLLHUP;

	port = tty->port;
	flags = spin_lock_irqsave(&port->lock);
	if (tty->eof_pending) {
		rev |= POLLIN | POLLRDNORM;
	} else if (tty->rx_w != tty->rx_r) {
		bool ready = true;

		if (port->termios.c_lflag & ICANON) {
			size_t avail = tty->rx_w - tty->rx_r;

			ready = avail >= tty->rx_size;
			for (size_t i = 0; !ready && i < avail; i++) {
				if (tty->rx_buf[(tty->rx_r + i) % tty->rx_size] ==
				    '\n')
					ready = true;
			}
		}
		if (ready)
			rev |= POLLIN | POLLRDNORM;
	}
	rev |= POLLOUT | POLLWRNORM;
	spin_unlock_irqrestore(&port->lock, flags);

	return rev & (events | POLLERR | POLLHUP);
}

const struct fs_file_ops tty_fops = {
	.open = tty_file_open,
	.release = tty_file_release,
	.read = tty_file_read,
	.write = tty_file_write,
	.llseek = tty_file_llseek,
	.iterate_shared = tty_file_iterate_shared,
	.fsync = tty_file_sync,
	.flush = tty_file_flush,
	.ioctl = tty_file_ioctl,
	.poll = tty_file_poll,
};
