#include <brk/assert.h>
#include <brk/blkdev.h>
#include <brk/devno.h>
#include <brk/kmalloc.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/spinlock.h>
#include <inttypes.h>
#include <uapi/errno.h>

#include "blkdev_internal.h"

static struct devno_allocator bd_devno;
static struct hlist_head bd_htable[MAJOR_MAX];
static SPINLOCK_DEFINE(bd_htable_lock);

void blkdev_registry_init(void)
{
	devno_allocator_init(&bd_devno, FIRST_DYNAMIC_BLKDEV_MAJOR);
}

int blkdev_register(struct block_dev *bd)
{
	dev_t dev;

	if (!bd)
		return -EINVAL;
	dev = bd->devt;
	if (!IS_BLKDEV(dev))
		return -EINVAL;
	if (!devno_minor_is_allocated(&bd_devno, MAJOR(dev), MINOR(dev)))
		return -EINVAL;

	pr_debug("%s: devtype=%" PRIu32 ", major=%u, minor=%u\n", __func__,
		 DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spin_lock(&bd_htable_lock);
	if (!hlist_unhashed(&bd->hlist)) {
		spin_unlock(&bd_htable_lock);
		return -EBUSY;
	}
	bd->dead = false;
	hlist_add_head(&bd->hlist, &bd_htable[MAJOR(dev)]);
	spin_unlock(&bd_htable_lock);

	return 0;
}

static struct block_dev *blkdev_get_no_lock(dev_t dev)
{
	struct block_dev *bd;

	hlist_for_each_entry(bd, &bd_htable[MAJOR(dev)], hlist) {
		if (bd->devt == dev)
			return bd;
	}

	return NULL;
}

void blkdev_unregister(struct block_dev *bd)
{
	if (!bd)
		return;

	spin_lock(&bd_htable_lock);
	bd->dead = true;
	if (!hlist_unhashed(&bd->hlist))
		hlist_del_init(&bd->hlist);
	spin_unlock(&bd_htable_lock);
}

struct block_dev *blkdev_get(dev_t dev)
{
	struct block_dev *bd;

	pr_debug("%s: devtype=%" PRIu32 ", major=%u, minor=%u\n", __func__,
		 DEVTYPE(dev), MAJOR(dev), MINOR(dev));

	spin_lock(&bd_htable_lock);
	bd = blkdev_get_no_lock(dev);
	if (bd && (bd->dead || !refcnt_inc_unless_zero(&bd->ref)))
		bd = NULL;
	spin_unlock(&bd_htable_lock);
	return bd;
}

void blkdev_put(struct block_dev *bd)
{
	struct block_dev *parent;

	if (!bd)
		return;
	if (refcnt_dec_fetch(&bd->ref) > 0)
		return;

	assert(hlist_unhashed(&bd->hlist));
	parent = bd->bd_parent;
	page_cache_destroy(bd->bd_mapping);
	bd->bd_mapping = NULL;
	kfree(bd);

	/* A partition view pins the whole-disk block_dev. */
	blkdev_put(parent);
}

int blkdev_alloc_major(unsigned *major_out)
{
	return devno_alloc_major(&bd_devno, major_out);
}

void blkdev_free_major(unsigned major)
{
	devno_free_major(&bd_devno, major);
}

int blkdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	return devno_alloc_minor(&bd_devno, major, minor_out);
}

void blkdev_free_minor(unsigned major, unsigned minor)
{
	devno_free_minor(&bd_devno, major, minor);
}

int blkdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out)
{
	return devno_alloc_region(&bd_devno, major, base_minor, count, dev_out);
}

void blkdev_free_region(unsigned major, unsigned minor, unsigned count)
{
	devno_free_region(&bd_devno, major, minor, count);
}

struct block_dev *blkdev_alloc(void)
{
	struct block_dev *bd = kzalloc(sizeof(*bd));
	if (!bd)
		return NULL;

	bd->bd_mapping = page_cache_create(bd, &bdev_pc_ops);
	if (!bd->bd_mapping) {
		kfree(bd);
		return NULL;
	}
	refcnt_init(&bd->ref, 1);
	hlist_node_init(&bd->hlist);
	bd->dead = false;
	return bd;
}

void blkdev_free(struct block_dev *bd)
{
	blkdev_put(bd);
}
