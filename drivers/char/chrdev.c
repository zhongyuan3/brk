#include <brk/chrdev.h>
#include <brk/device.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/types.h>

static struct dev_map cd_map;
static struct hlist_head cd_htable[MAJOR_MAX];
static SPINLOCK_DEFINE(cd_htable_lock);

void chrdev_registry_init(void)
{
	dev_map_init(&cd_map, FIRST_DYNAMIC_CHRDEV_MAJOR);
}

struct char_dev *chrdev_alloc(void)
{
	return kzalloc(sizeof(struct char_dev));
}

void chrdev_free(struct char_dev *cd)
{
	kfree(cd);
}

int chrdev_register(struct char_dev *cd)
{
	if (!cd || !IS_CHRDEV(cd->dev))
		return -EINVAL;
	if (!cd->fops || !cd->fops->open)
		return -EINVAL;
	if (!minor_is_allocated(&cd_map, MAJOR(cd->dev), MINOR(cd->dev)))
		return -EINVAL;

	spinlock_acquire(&cd_htable_lock);
	hlist_add_head(&cd->hlist, &cd_htable[MAJOR(cd->dev)]);
	spinlock_release(&cd_htable_lock);

	return 0;
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

void chrdev_unregister(struct char_dev *cd)
{
	spinlock_acquire(&cd_htable_lock);
	hlist_del_init(&cd->hlist);
	spinlock_release(&cd_htable_lock);
}

struct char_dev *chrdev_get(dev_t dev)
{
	spinlock_acquire(&cd_htable_lock);
	struct char_dev *cd = chrdev_get_no_lock(dev);
	spinlock_release(&cd_htable_lock);
	return cd;
}

int chrdev_alloc_major(unsigned *major_out)
{
	return alloc_dev_major(&cd_map, major_out);
}

void chrdev_free_major(unsigned major)
{
	free_dev_major(&cd_map, major);
}

int chrdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	return alloc_dev_minor(&cd_map, major, minor_out);
}

void chrdev_free_minor(unsigned major, unsigned minor)
{
	free_dev_minor(&cd_map, major, minor);
}

int chrdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out)
{
	return alloc_dev_region(&cd_map, major, base_minor, count, dev_out);
}

void chrdev_free_region(unsigned major, unsigned minor, unsigned count)
{
	free_dev_region(&cd_map, major, minor, count);
}

static int chrdev_open(struct fs_inode *inode, struct opened_file *file)
{
	struct char_dev *cd;

	cd = chrdev_get(inode->i_rdev);
	if (!cd || !cd->fops || !cd->fops->open)
		return -ENODEV;

	return cd->fops->open(inode, file);
}

const struct opened_file_ops chrdev_fops = {
	.open = chrdev_open,
};
