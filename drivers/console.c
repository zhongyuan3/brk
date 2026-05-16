#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/tty.h>

static int console_open(struct chrdev *cd, struct file *file)
{
	struct tty_file_priv *priv;

	(void)cd;

	priv = tty_file_priv_create();
	if (!priv)
		return -ENOMEM;
	file->private_data = priv;
	return 0;
}

static void console_release(struct chrdev *cd, struct file *file)
{
	(void)cd;

	if (file->private_data) {
		tty_file_priv_destroy(file->private_data);
		file->private_data = NULL;
	}
}

static int console_read(struct file *file, char *buf, usize_t n, usize_t *read)
{
	return tty_read(tty_boot(), file, buf, n, read);
}

static int console_write(struct file *file, const char *buf, usize_t n,
			 usize_t *written)
{
	(void)file;
	return tty_write(tty_boot(), buf, n, written);
}

static long console_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return tty_ioctl(tty_boot(), file, cmd, arg);
}

int dev_console_init(void)
{
	struct chrdev *cd;
	int err;

	cd = chrdev_alloc();
	if (!cd)
		return -ENOMEM;

	cd->ops.open = console_open;
	cd->ops.release = console_release;
	cd->ops.read = console_read;
	cd->ops.write = console_write;
	cd->ops.ioctl = console_ioctl;

	err = chrdev_register(cd, DEV_CONSOLE0);
	if (err) {
		chrdev_free(cd);
		return err;
	}
	return 0;
}
