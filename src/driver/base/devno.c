#include <brk/devno.h>
#include <brk/kmalloc.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/types.h>
#include <uapi/errno.h>

void devno_allocator_init(struct devno_allocator *a, unsigned int first_dynamic)
{
	bitmap_zero(a->major_pool, MAJOR_MAX);
	/* Static majors below first_dynamic are reserved but not allocatable. */
	bitmap_fill(a->major_pool, first_dynamic);

	for (unsigned int i = 0; i < MAJOR_MAX; ++i)
		hlist_head_init(&a->regions[i]);

	spinlock_init(&a->lock, "devno");
}

static int devno_alloc_major_no_lock(struct devno_allocator *a,
				     unsigned int *major_out)
{
	size_t bit = 0;

	if (!bitmap_alloc_bit_from(a->major_pool, MAJOR_MAX, 0, &bit))
		return -ENOMEM;

	*major_out = (unsigned int)bit;
	return 0;
}

static void devno_free_major_no_lock(struct devno_allocator *a,
				     unsigned int major)
{
	bitmap_free_bit(a->major_pool, MAJOR_MAX, major);
}

static int devno_alloc_region_no_lock(struct devno_allocator *a,
				      unsigned int major,
				      unsigned int base_minor,
				      unsigned int count, dev_t *dev_out)
{
	struct devno_region *curr;
	struct devno_region *new_entry;
	struct devno_region *last = NULL;
	bool major_allocated = false;
	unsigned int minor = base_minor;

	if (major == 0) {
		int err = devno_alloc_major_no_lock(a, &major);

		if (err)
			return err;
		major_allocated = true;
	} else if (major >= MAJOR_MAX ||
		   !bitmap_test_bit(a->major_pool, major)) {
		return -EINVAL;
	}

	if (count == 0 || count > MINOR_MAX || base_minor >= MINOR_MAX)
		goto err_range;

	hlist_for_each_entry(curr, &a->regions[major], entry) {
		unsigned int start = curr->minor_start;
		unsigned int end = start + curr->minor_count;

		/*
		 * Regions are kept sorted by minor_start.  Find the first gap
		 * at or after @minor that is large enough for @count.
		 */
		if (minor < start) {
			if (start - minor >= count)
				goto found;
		}

		if (minor < end)
			minor = end;
		last = curr;
	}

	if (minor >= MINOR_MAX || count > MINOR_MAX - minor)
		goto err_range;

found:
	new_entry = kzalloc(sizeof(*new_entry));
	if (!new_entry)
		goto err_nomem;

	new_entry->minor_start = minor;
	new_entry->minor_count = count;
	if (last)
		hlist_add_behind(&new_entry->entry, &last->entry);
	else
		hlist_add_head(&new_entry->entry, &a->regions[major]);

	*dev_out = MKDEV(major, minor);
	return 0;

err_nomem:
	if (major_allocated)
		devno_free_major_no_lock(a, major);
	return -ENOMEM;

err_range:
	if (major_allocated)
		devno_free_major_no_lock(a, major);
	return -EINVAL;
}

int devno_alloc_major(struct devno_allocator *a, unsigned int *major_out)
{
	int ret;

	if (!major_out)
		return -EINVAL;

	spin_lock(&a->lock);
	ret = devno_alloc_major_no_lock(a, major_out);
	spin_unlock(&a->lock);
	return ret;
}

void devno_free_major(struct devno_allocator *a, unsigned int major)
{
	if (major >= MAJOR_MAX)
		return;

	spin_lock(&a->lock);
	devno_free_major_no_lock(a, major);
	spin_unlock(&a->lock);
}

int devno_alloc_minor(struct devno_allocator *a, unsigned int major,
		      unsigned int *minor_out)
{
	dev_t dev = 0;
	int err;

	if (major >= MAJOR_MAX || !minor_out)
		return -EINVAL;

	spin_lock(&a->lock);
	if (!bitmap_test_bit(a->major_pool, major)) {
		spin_unlock(&a->lock);
		return -EINVAL;
	}
	err = devno_alloc_region_no_lock(a, major, 0, 1, &dev);
	spin_unlock(&a->lock);

	if (err)
		return err;
	*minor_out = MINOR(dev);
	return 0;
}

void devno_free_minor(struct devno_allocator *a, unsigned int major,
		      unsigned int minor)
{
	devno_free_region(a, major, minor, 1);
}

int devno_alloc_region(struct devno_allocator *a, unsigned int major,
		       unsigned int base_minor, unsigned int count,
		       dev_t *dev_out)
{
	int ret;

	if (major >= MAJOR_MAX || count == 0 || !dev_out)
		return -EINVAL;

	spin_lock(&a->lock);
	ret = devno_alloc_region_no_lock(a, major, base_minor, count,
					 dev_out);
	spin_unlock(&a->lock);
	return ret;
}

void devno_free_region(struct devno_allocator *a, unsigned int major,
		       unsigned int minor, unsigned int count)
{
	struct devno_region *curr;

	if (major >= MAJOR_MAX || count == 0 || minor >= MINOR_MAX ||
	    count > MINOR_MAX - minor)
		return;

	spin_lock(&a->lock);
	hlist_for_each_entry(curr, &a->regions[major], entry) {
		if (curr->minor_start + curr->minor_count <= minor)
			continue;

		if (curr->minor_start == minor && curr->minor_count == count) {
			hlist_del_init(&curr->entry);
			spin_unlock(&a->lock);
			kfree(curr);
			return;
		}

		pr_err("%s: region %u+%u does not match %u+%u\n", __func__,
		       curr->minor_start, curr->minor_count, minor, count);
		spin_unlock(&a->lock);
		return;
	}
	spin_unlock(&a->lock);
	pr_err("%s: no region at minor %u count %u\n", __func__, minor,
	       count);
}

bool devno_minor_is_allocated(struct devno_allocator *a, unsigned int major,
			      unsigned int minor)
{
	struct devno_region *curr;
	bool found = false;

	if (major >= MAJOR_MAX || minor >= MINOR_MAX)
		return false;

	spin_lock(&a->lock);
	if (bitmap_test_bit(a->major_pool, major)) {
		hlist_for_each_entry(curr, &a->regions[major], entry) {
			if (curr->minor_start <= minor &&
			    curr->minor_start + curr->minor_count > minor) {
				found = true;
				break;
			}
		}
	}
	spin_unlock(&a->lock);
	return found;
}
