#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/lock.h>
#include <brk/slab.h>
#include <brk/types.h>

static struct dev_registry chrdev_registry;

struct chrdev *chrdev_alloc(void)
{
	return kzalloc(sizeof(struct chrdev));
}

void chrdev_free(struct chrdev *cd)
{
	kfree(cd);
}

int chrdev_register(struct chrdev *cd, dev_t dev)
{
	if (!IS_CHRDEV(dev))
		return -EINVAL;
	return dev_registry_register(&chrdev_registry, (struct dev_slot *)cd,
				     dev, CHRDEV);
}

void chrdev_unregister(struct chrdev *cd)
{
	dev_registry_unregister(&chrdev_registry, (struct dev_slot *)cd);
}

struct chrdev *chrdev_get(dev_t dev)
{
	return (struct chrdev *)dev_registry_get(&chrdev_registry, dev);
}

int chrdev_alloc_major(unsigned *major_out)
{
	return dev_registry_alloc_major(&chrdev_registry, major_out);
}

void chrdev_free_major(unsigned major)
{
	dev_registry_free_major(&chrdev_registry, major);
}

int chrdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	return dev_registry_alloc_minor(&chrdev_registry, major, minor_out,
					CHRDEV);
}

int chrdev_alloc_devnum(dev_t *dev_out)
{
	return dev_registry_alloc_devnum(&chrdev_registry, dev_out, CHRDEV);
}

void chrdev_registry_init(void)
{
	dev_registry_init(&chrdev_registry);
}

static int chrdev_open(struct inode *inode, struct file *file)
{
	struct chrdev *cd;
	int err = 0;

	file->f_op = &chrdev_fops;

	cd = chrdev_get(inode->i_rdev);
	if (!cd)
		return -ENODEV;

	if (cd->ops.open)
		err = cd->ops.open(cd, file);
	return err;
}

static int chrdev_release(struct inode *inode, struct file *file)
{
	struct chrdev *cd;

	cd = chrdev_get(inode->i_rdev);
	if (cd && cd->ops.release)
		cd->ops.release(cd, file);
	return 0;
}

static ssize_t chrdev_read(struct file *file, char *buf, usize_t size,
			   loff_t *pos)
{
	(void)pos;
	struct inode *ip = file->f_inode;
	struct chrdev *cd;
	int err;
	usize_t rcnt = 0;

	sleeplock_acquire(&ip->i_rwsem);

	cd = chrdev_get(ip->i_rdev);
	if (!cd) {
		err = -ENODEV;
		goto unlock_and_out;
	}

	if (!cd->ops.read) {
		err = -EOPNOTSUPP;
		goto unlock_and_out;
	}

	err = cd->ops.read(file, buf, size, &rcnt);

unlock_and_out:
	sleeplock_release(&ip->i_rwsem);
	if (err)
		return err;
	return rcnt;
}

static ssize_t chrdev_write(struct file *file, const char *buf, usize_t size,
			    loff_t *pos)
{
	(void)pos;
	struct inode *ip = file->f_inode;
	struct chrdev *cd;
	int err;
	usize_t wcnt = 0;

	sleeplock_acquire(&ip->i_rwsem);

	cd = chrdev_get(ip->i_rdev);
	if (!cd) {
		err = -ENODEV;
		goto unlock_and_out;
	}

	if (!cd->ops.write) {
		err = -EOPNOTSUPP;
		goto unlock_and_out;
	}

	err = cd->ops.write(file, buf, size, &wcnt);

unlock_and_out:
	sleeplock_release(&ip->i_rwsem);
	if (err)
		return err;
	return wcnt;
}

static loff_t chrdev_llseek(struct file *file, loff_t offset, int whence)
{
	(void)whence;
	(void)offset;
	(void)file;
	return 0;
}

static int chrdev_iterate_shared(struct file *file, struct dir_context *ctx)
{
	(void)file;
	(void)ctx;
	return 0;
}

static int chrdev_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int chrdev_flush(struct file *file)
{
	(void)file;
	return 0;
}

static long chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *ip = file->f_inode;
	struct chrdev *cd;
	long ret = -ENODEV;

	sleeplock_acquire(&ip->i_rwsem);
	cd = chrdev_get(ip->i_rdev);
	if (cd) {
		if (cd->ops.ioctl)
			ret = cd->ops.ioctl(file, cmd, arg);
		else
			ret = -ENOTTY;
	}
	sleeplock_release(&ip->i_rwsem);
	return ret;
}

const struct file_operations chrdev_fops = {
	.open = chrdev_open,
	.release = chrdev_release,
	.read = chrdev_read,
	.write = chrdev_write,
	.llseek = chrdev_llseek,
	.iterate_shared = chrdev_iterate_shared,
	.fsync = chrdev_fsync,
	.flush = chrdev_flush,
	.ioctl = chrdev_ioctl,
};
