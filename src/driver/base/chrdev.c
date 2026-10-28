#include <brk/assert.h>
#include <brk/chrdev.h>
#include <brk/devno.h>
#include <brk/device.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/initcall.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/types.h>

static struct devno_allocator cd_devno;
static struct hlist_head cd_htable[MAJOR_MAX];
static SPINLOCK_DEFINE(cd_htable_lock);

void chrdev_registry_init(void)
{
	devno_allocator_init(&cd_devno, FIRST_DYNAMIC_CHRDEV_MAJOR);
}
core_initcall(chrdev_registry_init);

struct char_dev *chrdev_alloc(void)
{
	struct char_dev *cd = kzalloc(sizeof(*cd));

	if (!cd)
		return NULL;

	refcnt_init(&cd->ref, 1);
	hlist_node_init(&cd->hlist);
	cd->dead = false;
	return cd;
}

void chrdev_put(struct char_dev *cd)
{
	if (!cd)
		return;
	if (refcnt_dec_fetch(&cd->ref) > 0)
		return;

	ASSERT(hlist_unhashed(&cd->hlist));
	kfree(cd);
}

void chrdev_free(struct char_dev *cd)
{
	chrdev_put(cd);
}

int chrdev_register(struct char_dev *cd)
{
	if (!cd || !IS_CHRDEV(cd->dev))
		return -EINVAL;
	if (!cd->fops || !cd->fops->open)
		return -EINVAL;
	if (!devno_minor_is_allocated(&cd_devno, MAJOR(cd->dev),
				      MINOR(cd->dev)))
		return -EINVAL;

	spin_lock(&cd_htable_lock);
	if (!hlist_unhashed(&cd->hlist)) {
		spin_unlock(&cd_htable_lock);
		return -EBUSY;
	}
	cd->dead = false;
	hlist_add_head(&cd->hlist, &cd_htable[MAJOR(cd->dev)]);
	spin_unlock(&cd_htable_lock);
	return 0;
}

void chrdev_unregister(struct char_dev *cd)
{
	if (!cd)
		return;

	spin_lock(&cd_htable_lock);
	cd->dead = true;
	if (!hlist_unhashed(&cd->hlist))
		hlist_del_init(&cd->hlist);
	spin_unlock(&cd_htable_lock);
}

static struct char_dev *chrdev_get_no_lock(dev_t dev)
{
	struct char_dev *cd;

	hlist_for_each_entry(cd, &cd_htable[MAJOR(dev)], hlist) {
		if (cd->dev == dev)
			return cd;
	}
	return NULL;
}

struct char_dev *chrdev_get(dev_t dev)
{
	struct char_dev *cd;

	spin_lock(&cd_htable_lock);
	cd = chrdev_get_no_lock(dev);
	if (cd && (cd->dead || !refcnt_inc_unless_zero(&cd->ref)))
		cd = NULL;
	spin_unlock(&cd_htable_lock);
	return cd;
}

int chrdev_alloc_major(unsigned *major_out)
{
	return devno_alloc_major(&cd_devno, major_out);
}

void chrdev_free_major(unsigned major)
{
	devno_free_major(&cd_devno, major);
}

int chrdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	return devno_alloc_minor(&cd_devno, major, minor_out);
}

void chrdev_free_minor(unsigned major, unsigned minor)
{
	devno_free_minor(&cd_devno, major, minor);
}

int chrdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out)
{
	return devno_alloc_region(&cd_devno, major, base_minor, count,
				  dev_out);
}

void chrdev_free_region(unsigned major, unsigned minor, unsigned count)
{
	devno_free_region(&cd_devno, major, minor, count);
}

static struct char_dev *chrdev_file_dev(struct fs_file *file)
{
	return (struct char_dev *)file->dev_private;
}

static int chrdev_open(struct fs_inode *inode, struct fs_file *file)
{
	struct char_dev *cd;
	int err;

	cd = chrdev_get(inode->rdev);
	if (!cd || !cd->fops || !cd->fops->open) {
		if (cd)
			chrdev_put(cd);
		return -ENODEV;
	}

	file->ops = &chrdev_fops;
	file->dev_private = cd;

	err = cd->fops->open(inode, file);
	if (err) {
		file->dev_private = NULL;
		chrdev_put(cd);
	}
	return err;
}

static int chrdev_release(struct fs_inode *inode, struct fs_file *file)
{
	struct char_dev *cd = chrdev_file_dev(file);
	int err = 0;

	if (!cd)
		return 0;

	if (cd->fops && cd->fops->release)
		err = cd->fops->release(inode, file);

	file->dev_private = NULL;
	chrdev_put(cd);
	return err;
}

static ssize_t chrdev_read(struct fs_file *file, char *buf, size_t size,
			   loff_t *pos)
{
	struct char_dev *cd = chrdev_file_dev(file);

	if (!cd || !cd->fops || !cd->fops->read)
		return -EOPNOTSUPP;
	return cd->fops->read(file, buf, size, pos);
}

static ssize_t chrdev_write(struct fs_file *file, const char *buf,
			    size_t size, loff_t *pos)
{
	struct char_dev *cd = chrdev_file_dev(file);

	if (!cd || !cd->fops || !cd->fops->write)
		return -EOPNOTSUPP;
	return cd->fops->write(file, buf, size, pos);
}

static loff_t chrdev_llseek(struct fs_file *file, loff_t offset, int whence)
{
	struct char_dev *cd = chrdev_file_dev(file);

	if (!cd || !cd->fops || !cd->fops->llseek)
		return -EOPNOTSUPP;
	return cd->fops->llseek(file, offset, whence);
}

static int chrdev_iterate_shared(struct fs_file *file,
				 struct fs_dir_iterator *ctx)
{
	struct char_dev *cd = chrdev_file_dev(file);

	if (!cd || !cd->fops || !cd->fops->iterate_shared)
		return -ENOTDIR;
	return cd->fops->iterate_shared(file, ctx);
}

static int chrdev_fsync(struct fs_file *file, loff_t start, loff_t end,
			int datasync)
{
	struct char_dev *cd = chrdev_file_dev(file);

	if (!cd || !cd->fops || !cd->fops->fsync)
		return 0;
	return cd->fops->fsync(file, start, end, datasync);
}

static int chrdev_flush(struct fs_file *file)
{
	struct char_dev *cd = chrdev_file_dev(file);

	if (!cd || !cd->fops || !cd->fops->flush)
		return 0;
	return cd->fops->flush(file);
}

static long chrdev_ioctl(struct fs_file *file, unsigned int cmd,
			 unsigned long arg)
{
	struct char_dev *cd = chrdev_file_dev(file);

	if (!cd || !cd->fops || !cd->fops->ioctl)
		return -ENOTTY;
	return cd->fops->ioctl(file, cmd, arg);
}

const struct fs_file_ops chrdev_fops = {
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
