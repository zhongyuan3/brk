#include <brk/chrdev.h>
#include <brk/device.h>
#include <brk/fs.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/fcntl.h>
#include <uapi/types.h>

static HLIST_HEAD_DEFINE(tty_driver_list);
static SPINLOCK_DEFINE(tty_driver_lock);
static const struct file_ops tty_fops;

struct tty_driver *tty_alloc_driver(int num_ports)
{
	struct tty_driver *driver;

	driver = kzalloc(sizeof(struct tty_driver));
	if (!driver)
		goto err_alloc_driver;
	spinlock_init(&driver->lock, "tty_driver");
	driver->num_ports = num_ports;
	driver->ports = kcalloc(num_ports, sizeof(driver->ports[0]));
	if (!driver->ports)
		goto err_alloc_ports;
	driver->cds = kcalloc(num_ports, sizeof(driver->cds[0]));
	if (!driver->cds)
		goto err_alloc_cds;

	return driver;

err_alloc_cds:
	kfree(driver->ports);
err_alloc_ports:
	kfree(driver);
err_alloc_driver:
	return NULL;
}

void tty_free_driver(struct tty_driver *driver)
{
	kfree(driver->cds);
	kfree(driver->ports);
	kfree(driver);
}

/* Caller must hold driver->lock. */
static void tty_driver_cleanup_port(struct tty_driver *driver, int i)
{
	struct tty_port *port = driver->ports[i];
	struct char_dev *cd = driver->cds[i];

	if (!port && !cd)
		return;

	driver->ports[i] = NULL;
	driver->cds[i] = NULL;

	if (port) {
		port->driver = NULL;
		tty_detach_port(port);
	}
	if (cd) {
		chrdev_unregister(cd);
		chrdev_free(cd);
	}
}

int tty_register_driver(struct tty_driver *driver)
{
	dev_t dev = 0;
	int err = 0;

	if (!driver || !driver->name || !driver->ops)
		return -EINVAL;

	if (driver->major == 0) {
		err = chrdev_alloc_region(0, 0, driver->num_ports, &dev);
		if (err)
			return err;
		driver->major = MAJOR(dev);
		driver->minor_start = MINOR(dev);
	}

	spinlock_acquire(&tty_driver_lock);
	hlist_add_head(&driver->driver_list, &tty_driver_list);
	spinlock_release(&tty_driver_lock);
	return 0;
}

int tty_unregister_driver(struct tty_driver *driver)
{
	if (!driver)
		return -EINVAL;

	spinlock_acquire(&driver->lock);
	for (int i = 0; i < driver->num_ports; i++) {
		struct tty_port *port = driver->ports[i];

		if (port && port->tty && refcnt_read(&port->tty->refcnt) > 0) {
			spinlock_release(&driver->lock);
			return -EBUSY;
		}
	}
	spinlock_release(&driver->lock);

	spinlock_acquire(&tty_driver_lock);
	hlist_del(&driver->driver_list);
	spinlock_release(&tty_driver_lock);

	spinlock_acquire(&driver->lock);
	for (int i = 0; i < driver->num_ports; i++)
		tty_driver_cleanup_port(driver, i);
	spinlock_release(&driver->lock);

	chrdev_free_region(driver->major, driver->minor_start,
			   driver->num_ports);
	tty_free_driver(driver);
	return 0;
}

struct tty_port *tty_lookup_port(dev_t dev)
{
	struct tty_driver *driver;
	unsigned major = MAJOR(dev);
	unsigned minor = MINOR(dev);
	struct tty_port *port = NULL;

	spinlock_acquire(&tty_driver_lock);
	hlist_for_each_entry(driver, &tty_driver_list, driver_list) {
		if (driver->major == major && driver->minor_start <= minor &&
		    driver->minor_start + driver->num_ports > minor) {
			port = driver->ports[minor - driver->minor_start];
			break;
		}
	}
	spinlock_release(&tty_driver_lock);

	return port;
}

int tty_driver_add_port(struct tty_driver *driver, struct tty_port *port)
{
	int err;

	if (!driver || !port)
		return -EINVAL;

	struct char_dev *cd = chrdev_alloc();
	if (!cd)
		return -ENOMEM;

	spinlock_acquire(&driver->lock);
	for (int i = 0; i < driver->num_ports; i++) {
		if (driver->ports[i] == NULL) {
			driver->ports[i] = port;
			cd->fops = &tty_fops;
			cd->dev = MKCHRDEV(driver->major,
					   driver->minor_start + i);
			driver->cds[i] = cd;
			err = chrdev_register(cd);
			if (err) {
				chrdev_free(cd);
				driver->cds[i] = NULL;
				driver->ports[i] = NULL;
				spinlock_release(&driver->lock);
				return err;
			}
			if (!tty_attach_port(port)) {
				chrdev_unregister(cd);
				chrdev_free(cd);
				driver->cds[i] = NULL;
				driver->ports[i] = NULL;
				spinlock_release(&driver->lock);
				return -ENOMEM;
			}
			spinlock_release(&driver->lock);
			return 0;
		}
	}
	spinlock_release(&driver->lock);

	chrdev_free(cd);

	return -EBUSY;
}

int tty_driver_remove_port(struct tty_driver *driver, struct tty_port *port)
{
	if (!driver || !port)
		return -EINVAL;

	spinlock_acquire(&driver->lock);
	if (port->tty && refcnt_read(&port->tty->refcnt) > 0) {
		spinlock_release(&driver->lock);
		return -EBUSY;
	}

	for (int i = 0; i < driver->num_ports; i++) {
		if (driver->ports[i] == port) {
			tty_driver_cleanup_port(driver, i);
			spinlock_release(&driver->lock);
			return 0;
		}
	}
	spinlock_release(&driver->lock);

	return -ENOENT;
}

static int tty_file_open(struct inode *inode, struct file *file)
{
	file->f_op = &tty_fops;
	file->private_data = NULL;

	struct tty_port *port = tty_lookup_port(inode->i_rdev);
	if (!port)
		return -ENXIO;

	struct tty *tty = tty_open(port);
	if (!tty)
		return -ENOMEM;

	file->private_data = tty;
	port->foreground = current_process();
	return 0;
}

static int tty_file_release(struct inode *inode, struct file *file)
{
	(void)inode;

	struct tty *tty = file->private_data;
	if (!tty)
		return -EINVAL;

	tty_close(tty);
	return 0;
}

static ssize_t tty_file_read(struct file *file, char *buf, usize_t size,
			     loff_t *pos)
{
	(void)pos;
	struct tty *tty = file->private_data;
	if (!tty)
		return -EINVAL;
	return tty_read(tty, buf, size);
}

static ssize_t tty_file_write(struct file *file, const char *buf, usize_t size,
			      loff_t *pos)
{
	(void)pos;
	struct tty *tty = file->private_data;
	if (!tty)
		return -EINVAL;
	return tty_write(tty, buf, size);
}

static loff_t tty_file_llseek(struct file *file, loff_t offset, int whence)
{
	(void)whence;
	(void)offset;
	(void)file;
	return 0;
}

static int tty_file_iterate_shared(struct file *file, struct dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int tty_file_sync(struct file *file, loff_t start, loff_t end,
			 int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int tty_file_flush(struct file *file)
{
	(void)file;
	return 0;
}

static long tty_file_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct tty *tty = file->private_data;

	return tty_ioctl(tty, cmd, arg);
}

static const struct file_ops tty_fops = {
	.open = tty_file_open,
	.release = tty_file_release,
	.read = tty_file_read,
	.write = tty_file_write,
	.llseek = tty_file_llseek,
	.iterate_shared = tty_file_iterate_shared,
	.fsync = tty_file_sync,
	.flush = tty_file_flush,
	.ioctl = tty_file_ioctl,
};

int tty_create_fs_nodes(void)
{
	int err = 0;
	struct tty_driver *driver;
	char name[32];

	spinlock_acquire(&tty_driver_lock);
	hlist_for_each_entry(driver, &tty_driver_list, driver_list) {
		for (int i = 0; i < driver->num_ports; i++) {
			if (driver->ports[i]) {
				memset(name, 0, sizeof(name));
				snprintf(name, sizeof(name) - 1, "/dev/tty%u",
					 i);
				err = do_mknodat(
					AT_FDCWD, name, S_IFCHR,
					MKCHRDEV(driver->major,
						 driver->minor_start + i));
				if (err) {
					spinlock_release(&tty_driver_lock);
					return err;
				}
				klog_info("/dev/tty%u created successfully\n",
					  i);
			}
		}
	}
	spinlock_release(&tty_driver_lock);
	return 0;
}
