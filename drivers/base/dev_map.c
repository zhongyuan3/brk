#include <brk/bitmap.h>
#include <brk/device.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/types.h>

void dev_map_init(struct dev_map *map, unsigned reserved)
{
	if (map) {
		memset(map->entries, 0, sizeof(map->entries));
		bitmap_zero(map->major_pooled, MAJOR_MAX);
		spinlock_init(&map->lock, "dev_map");
		if (0 < reserved && reserved < MAJOR_MAX)
			bitmap_fill(map->major_pooled, reserved);
	}
}

static int alloc_dev_major_no_lock(struct dev_map *map, unsigned base_major,
				   unsigned *major_out)
{
	usize_t bit = 0;

	if (bitmap_alloc_bit_from(map->major_pooled, MAJOR_MAX, base_major,
				  &bit)) {
		*major_out = bit;
		return 0;
	}

	return -ENOMEM;
}

static void free_dev_major_no_lock(struct dev_map *map, unsigned major)
{
	if (!bitmap_test_bit(map->major_pooled, major)) {
		klog_error("%s: major %u is not allocated", __func__, major);
		return;
	}
	if (!hlist_empty(&map->entries[major])) {
		klog_error("%s: major %u is not empty", __func__, major);
		return;
	}
	bitmap_free_bit(map->major_pooled, MAJOR_MAX, major);
}

static int alloc_dev_region_no_lock(struct dev_map *map, unsigned major,
				    unsigned base_minor, unsigned count,
				    dev_t *dev_out)
{
	struct dev_map_entry *curr, *new_entry;
	int err = 0;
	bool major_allocated = false;
	unsigned minor = base_minor;
	struct dev_map_entry *last = NULL;

	if (major == 0) {
		err = alloc_dev_major_no_lock(map, 0, &major);
		if (err)
			return err;
		major_allocated = true;
	} else {
		if (!bitmap_test_bit(map->major_pooled, major))
			return -EINVAL;
	}

	hlist_for_each_entry(curr, &map->entries[major], entry) {
		if (curr->minor_start > minor &&
		    curr->minor_start - minor >= count) {
			new_entry = kzalloc(sizeof(*new_entry));
			if (!new_entry) {
				if (major_allocated)
					free_dev_major_no_lock(map, major);
				return -ENOMEM;
			}
			new_entry->minor_start = minor;
			new_entry->count = count;
			hlist_add_before(&new_entry->entry, &curr->entry);
			*dev_out = MKDEV(major, minor);
			return 0;
		}
		minor = curr->minor_start + curr->count;
		last = curr;
	}

	new_entry = kzalloc(sizeof(*new_entry));
	if (!new_entry) {
		if (major_allocated)
			free_dev_major_no_lock(map, major);
		return -ENOMEM;
	}
	new_entry->minor_start = minor;
	new_entry->count = count;
	if (last)
		hlist_add_behind(&new_entry->entry, &last->entry);
	else
		hlist_add_head(&new_entry->entry, &map->entries[major]);
	*dev_out = MKDEV(major, minor);
	return 0;
}

int alloc_dev_major(struct dev_map *map, unsigned *major_out)
{
	if (!map || !major_out)
		return -EINVAL;
	spinlock_acquire(&map->lock);
	int err = alloc_dev_major_no_lock(map, 0, major_out);
	spinlock_release(&map->lock);
	return err;
}

void free_dev_major(struct dev_map *map, unsigned major)
{
	if (!map || major >= MAJOR_MAX)
		return;
	spinlock_acquire(&map->lock);
	free_dev_major_no_lock(map, major);
	spinlock_release(&map->lock);
}

int alloc_dev_minor(struct dev_map *map, unsigned major, unsigned *minor_out)
{
	dev_t dev = 0;
	int err = 0;

	if (!map || major >= MAJOR_MAX || !minor_out)
		return -EINVAL;

	spinlock_acquire(&map->lock);
	if (!bitmap_test_bit(map->major_pooled, major)) {
		spinlock_release(&map->lock);
		return -EINVAL;
	}
	err = alloc_dev_region_no_lock(map, major, 0, 1, &dev);
	spinlock_release(&map->lock);

	if (err)
		return err;
	*minor_out = MINOR(dev);
	return 0;
}

void free_dev_minor(struct dev_map *map, unsigned major, unsigned minor)
{
	if (!map || major >= MAJOR_MAX || minor >= MINOR_MAX)
		return;

	free_dev_region(map, major, minor, 1);
}

/**
 * dev_reg_alloc_region() - Allocate a contiguous region of device numbers
 * @map: The device map to allocate from
 * @major: The major number to allocate from
 * @base_minor: The base minor number to allocate from
 * @count: The number of device numbers to allocate
 * @dev_out: The device number to allocate
 *
 * This function allocates a contiguous region of device numbers from the
 * device map. The device numbers are allocated from the major number. If
 * the major number is 0, a new major number will be allocated.
 */
int alloc_dev_region(struct dev_map *map, unsigned major, unsigned base_minor,
		     unsigned count, dev_t *dev_out)
{
	int ret;

	if (!map || major >= MAJOR_MAX || count == 0 || !dev_out)
		return -EINVAL;

	spinlock_acquire(&map->lock);
	ret = alloc_dev_region_no_lock(map, major, base_minor, count, dev_out);
	spinlock_release(&map->lock);

	return ret;
}

void free_dev_region(struct dev_map *map, unsigned major, unsigned minor,
		     unsigned count)
{
	struct dev_map_entry *curr;

	if (!map || major >= MAJOR_MAX || minor + count > MINOR_MAX)
		return;

	spinlock_acquire(&map->lock);
	hlist_for_each_entry(curr, &map->entries[major], entry) {
		if (curr->minor_start + curr->count <= minor)
			continue;

		if (curr->minor_start == minor && curr->count == count) {
			hlist_del_init(&curr->entry);
			spinlock_release(&map->lock);
			kfree(curr);
			return;
		}

		klog_error("%s: region %u+%u does not match %u+%u", __func__,
			   curr->minor_start, curr->count, minor, count);
		spinlock_release(&map->lock);
		return;
	}
	spinlock_release(&map->lock);
	klog_error("%s: no region at minor %u count %u", __func__, minor,
		   count);
}

bool major_is_allocated(struct dev_map *map, unsigned major)
{
	if (!map || major >= MAJOR_MAX)
		return false;

	spinlock_acquire(&map->lock);
	bool is_allocated = bitmap_test_bit(map->major_pooled, major);
	spinlock_release(&map->lock);
	return is_allocated;
}

bool minor_is_allocated(struct dev_map *map, unsigned major, unsigned minor)
{
	if (!map || major >= MAJOR_MAX || minor >= MINOR_MAX)
		return false;

	spinlock_acquire(&map->lock);
	if (!bitmap_test_bit(map->major_pooled, major)) {
		spinlock_release(&map->lock);
		return false;
	}
	struct dev_map_entry *curr;
	bool is_allocated = false;
	hlist_for_each_entry(curr, &map->entries[major], entry) {
		if (curr->minor_start <= minor &&
		    curr->minor_start + curr->count > minor) {
			is_allocated = true;
			break;
		}
	}
	spinlock_release(&map->lock);
	return is_allocated;
}
