#include <brk/chrdev.h>
#include <brk/device.h>
#include <brk/device_model.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/poll.h>
#include <uapi/stat.h>
#include <uapi/types.h>

static HLIST_HEAD_DEFINE(tty_driver_list);
static SPINLOCK_DEFINE(tty_driver_lock);
static const struct fs_file_ops tty_fops;

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
	driver->devs = kcalloc(num_ports, sizeof(driver->devs[0]));
	if (!driver->devs)
		goto err_alloc_devs;

	return driver;

err_alloc_devs:
	kfree(driver->cds);
err_alloc_cds:
	kfree(driver->ports);
err_alloc_ports:
	kfree(driver);
err_alloc_driver:
	return NULL;
}

void tty_free_driver(struct tty_driver *driver)
{
	kfree(driver->devs);
	kfree(driver->cds);
	kfree(driver->ports);
	kfree(driver);
}

/* Caller must hold driver->lock. */
static void tty_driver_cleanup_port(struct tty_driver *driver, int i)
{
	struct tty_port *port = driver->ports[i];
	struct char_dev *cd = driver->cds[i];
	struct device *dev = driver->devs[i];

	if (!port && !cd && !dev)
		return;

	driver->ports[i] = NULL;
	driver->cds[i] = NULL;
	driver->devs[i] = NULL;

	if (port) {
		port->driver = NULL;
		tty_detach_port(port);
	}
	if (dev) {
		device_unregister(dev);
		kfree(dev);
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

	spin_lock(&tty_driver_lock);
	hlist_add_head(&driver->driver_list, &tty_driver_list);
	spin_unlock(&tty_driver_lock);
	return 0;
}

int tty_unregister_driver(struct tty_driver *driver)
{
	if (!driver)
		return -EINVAL;

	spin_lock(&driver->lock);
	for (int i = 0; i < driver->num_ports; i++) {
		struct tty_port *port = driver->ports[i];
		bool busy = false;

		if (port) {
			irqstate_t pflags = spin_lock_irqsave(&port->lock);

			busy = port->open_count > 0;
			spin_unlock_irqrestore(&port->lock, pflags);
		}
		if (busy) {
			spin_unlock(&driver->lock);
			return -EBUSY;
		}
	}
	spin_unlock(&driver->lock);

	spin_lock(&tty_driver_lock);
	hlist_del(&driver->driver_list);
	spin_unlock(&tty_driver_lock);

	spin_lock(&driver->lock);
	for (int i = 0; i < driver->num_ports; i++)
		tty_driver_cleanup_port(driver, i);
	spin_unlock(&driver->lock);

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

	spin_lock(&tty_driver_lock);
	hlist_for_each_entry(driver, &tty_driver_list, driver_list) {
		if (driver->major == major && driver->minor_start <= minor &&
		    driver->minor_start + driver->num_ports > minor) {
			port = driver->ports[minor - driver->minor_start];
			break;
		}
	}
	spin_unlock(&tty_driver_lock);

	return port;
}

int tty_driver_add_port(struct tty_driver *driver, struct tty_port *port)
{
	struct char_dev *cd;
	struct device *dev;
	char name[32];
	int slot = -1;
	int err;

	if (!driver || !port)
		return -EINVAL;

	cd = chrdev_alloc();
	if (!cd)
		return -ENOMEM;

	dev = kzalloc(sizeof(*dev));
	if (!dev) {
		chrdev_free(cd);
		return -ENOMEM;
	}

	/* Reserve a slot, but do the slow registration work outside the lock. */
	spin_lock(&driver->lock);
	for (int i = 0; i < driver->num_ports; i++) {
		if (driver->ports[i] == NULL) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		spin_unlock(&driver->lock);
		chrdev_free(cd);
		kfree(dev);
		return -EBUSY;
	}

	cd->fops = &tty_fops;
	cd->dev = MKCHRDEV(driver->major, driver->minor_start + slot);
	snprintf(name, sizeof(name), "tty%u", (unsigned)slot);
	device_init(dev, name);
	dev->devt = cd->dev;
	dev->mode = S_IFCHR | 0666;

	driver->ports[slot] = port;
	driver->cds[slot] = cd;
	driver->devs[slot] = dev;
	spin_unlock(&driver->lock);

	err = chrdev_register(cd);
	if (err)
		goto err_slot;

	if (!tty_attach_port(port)) {
		err = -ENOMEM;
		chrdev_unregister(cd);
		goto err_slot;
	}

	err = device_register(dev);
	if (err) {
		/*
		 * The tty is already usable; keep it alive and report the
		 * missing node rather than tearing down a working port.
		 */
		pr_warn("tty: cannot register device %s: %d\n", name, err);
	}

	return 0;

err_slot:
	spin_lock(&driver->lock);
	if (driver->ports[slot] == port)
		driver->ports[slot] = NULL;
	if (driver->cds[slot] == cd)
		driver->cds[slot] = NULL;
	if (driver->devs[slot] == dev)
		driver->devs[slot] = NULL;
	spin_unlock(&driver->lock);
	kfree(dev);
	chrdev_free(cd);
	return err;
}

int tty_driver_remove_port(struct tty_driver *driver, struct tty_port *port)
{
	if (!driver || !port)
		return -EINVAL;

	spin_lock(&driver->lock);
	irqstate_t pflags = spin_lock_irqsave(&port->lock);
	bool busy = port->open_count > 0;
	spin_unlock_irqrestore(&port->lock, pflags);
	if (busy) {
		spin_unlock(&driver->lock);
		return -EBUSY;
	}

	for (int i = 0; i < driver->num_ports; i++) {
		if (driver->ports[i] == port) {
			tty_driver_cleanup_port(driver, i);
			spin_unlock(&driver->lock);
			return 0;
		}
	}
	spin_unlock(&driver->lock);

	return -ENOENT;
}

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

static const struct fs_file_ops tty_fops = {
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

struct tty_port *tty_port_alloc(void)
{
	struct tty_port *port = kzalloc(sizeof(struct tty_port));

	if (!port)
		return NULL;
	spinlock_init(&port->lock, "tty_port");
	tty_termios_defaults(&port->termios);
	return port;
}

void tty_port_free(struct tty_port *port)
{
	kfree(port);
}
