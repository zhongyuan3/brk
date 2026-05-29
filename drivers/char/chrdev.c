#include <brk/bitmap.h>
#include <brk/chrdev.h>
#include <brk/device.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>
#include <uapi/types.h>

struct char_dev_region {
	struct hlist_node entry;
	unsigned minor_start;
	unsigned minor_count;
};

static struct hlist_head cd_regs[MAJOR_MAX];
static BITMAP_DECLARE(cd_major_pooled, MAJOR_MAX);
static SPINLOCK_DEFINE(cd_lock);
static struct hlist_head cd_htable[MAJOR_MAX];
static SPINLOCK_DEFINE(cd_htable_lock);

static int chrdev_alloc_major_no_lock(unsigned base_major, unsigned *major_out)
{
	usize_t bit = 0;

	if (bitmap_alloc_bit_from(cd_major_pooled, MAJOR_MAX, base_major,
				  &bit)) {
		*major_out = bit;
		return 0;
	}

	return -ENOMEM;
}

static void chrdev_free_major_no_lock(unsigned major)
{
	bitmap_free_bit(cd_major_pooled, MAJOR_MAX, major);
}

static int chrdev_alloc_region_no_lock(unsigned major, unsigned base_minor,
				       unsigned count, dev_t *dev_out)
{
	struct char_dev_region *curr, *new_entry;
	int err = 0;
	bool major_allocated = false;
	unsigned minor = base_minor;
	struct char_dev_region *last = NULL;

	if (major == 0) {
		err = chrdev_alloc_major_no_lock(0, &major);
		if (err)
			return err;
		major_allocated = true;
	} else {
		if (!bitmap_test_bit(cd_major_pooled, major))
			return -EINVAL;
	}

	hlist_for_each_entry(curr, &cd_regs[major], entry) {
		if (curr->minor_start > minor &&
		    curr->minor_start - minor >= count) {
			new_entry = kzalloc(sizeof(*new_entry));
			if (!new_entry) {
				if (major_allocated)
					chrdev_free_major_no_lock(major);
				return -ENOMEM;
			}
			new_entry->minor_start = minor;
			new_entry->minor_count = count;
			hlist_add_before(&new_entry->entry, &curr->entry);
			*dev_out = MKDEV(major, minor);
			return 0;
		}
		minor = curr->minor_start + curr->minor_count;
		last = curr;
	}

	new_entry = kzalloc(sizeof(*new_entry));
	if (!new_entry) {
		if (major_allocated)
			chrdev_free_major_no_lock(major);
		return -ENOMEM;
	}
	new_entry->minor_start = minor;
	new_entry->minor_count = count;
	if (last)
		hlist_add_behind(&new_entry->entry, &last->entry);
	else
		hlist_add_head(&new_entry->entry, &cd_regs[major]);
	*dev_out = MKDEV(major, minor);
	return 0;
}

static bool chrdev_minor_is_allocated(unsigned major, unsigned minor)
{
	struct char_dev_region *curr;

	if (!bitmap_test_bit(cd_major_pooled, major))
		return false;

	hlist_for_each_entry(curr, &cd_regs[major], entry) {
		if (curr->minor_start <= minor &&
		    curr->minor_start + curr->minor_count > minor)
			return true;
	}
	return false;
}

void chrdev_registry_init(void)
{
	bitmap_fill(cd_major_pooled, FIRST_DYNAMIC_CHRDEV_MAJOR);
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
	spinlock_acquire(&cd_lock);
	if (!chrdev_minor_is_allocated(MAJOR(cd->dev), MINOR(cd->dev))) {
		spinlock_release(&cd_lock);
		return -EINVAL;
	}
	spinlock_release(&cd_lock);

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
	int err;

	if (!major_out)
		return -EINVAL;

	spinlock_acquire(&cd_lock);
	err = chrdev_alloc_major_no_lock(0, major_out);
	spinlock_release(&cd_lock);
	return err;
}

void chrdev_free_major(unsigned major)
{
	if (major >= MAJOR_MAX)
		return;

	spinlock_acquire(&cd_lock);
	chrdev_free_major_no_lock(major);
	spinlock_release(&cd_lock);
}

int chrdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	dev_t dev = 0;
	int err;

	if (major >= MAJOR_MAX || !minor_out)
		return -EINVAL;

	spinlock_acquire(&cd_lock);
	if (!bitmap_test_bit(cd_major_pooled, major)) {
		spinlock_release(&cd_lock);
		return -EINVAL;
	}
	err = chrdev_alloc_region_no_lock(major, 0, 1, &dev);
	spinlock_release(&cd_lock);

	if (err)
		return err;
	*minor_out = MINOR(dev);
	return 0;
}

void chrdev_free_minor(unsigned major, unsigned minor)
{
	chrdev_free_region(major, minor, 1);
}

int chrdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out)
{
	int ret;

	if (major >= MAJOR_MAX || count == 0 || !dev_out)
		return -EINVAL;

	spinlock_acquire(&cd_lock);
	ret = chrdev_alloc_region_no_lock(major, base_minor, count, dev_out);
	spinlock_release(&cd_lock);

	return ret;
}

void chrdev_free_region(unsigned major, unsigned minor, unsigned count)
{
	struct char_dev_region *curr;

	if (major >= MAJOR_MAX || minor + count > MINOR_MAX)
		return;

	spinlock_acquire(&cd_lock);
	hlist_for_each_entry(curr, &cd_regs[major], entry) {
		if (curr->minor_start + curr->minor_count <= minor)
			continue;

		if (curr->minor_start == minor && curr->minor_count == count) {
			hlist_del_init(&curr->entry);
			spinlock_release(&cd_lock);
			kfree(curr);
			return;
		}

		klog_error("%s: region %u+%u does not match %u+%u", __func__,
			   curr->minor_start, curr->minor_count, minor, count);
		spinlock_release(&cd_lock);
		return;
	}
	spinlock_release(&cd_lock);
	klog_error("%s: no region at minor %u count %u", __func__, minor,
		   count);
}

static int chrdev_open(struct fs_inode *inode, struct fs_file *file)
{
	struct char_dev *cd;

	cd = chrdev_get(inode->rdev);
	if (!cd || !cd->fops || !cd->fops->open)
		return -ENODEV;

	return cd->fops->open(inode, file);
}

const struct fs_file_ops chrdev_fops = {
	.open = chrdev_open,
};
