#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/kernel.h>

void dev_registry_init(struct dev_registry *reg)
{
	unsigned i;

	for (i = 0; i < BRK_MAJOR_MAX; i++)
		list_init(&reg->lists[i]);
	spinlock_init(&reg->lock, "dev_registry");
	bitmap_zero(reg->major_pooled, BRK_MAJOR_MAX);
}

static struct dev_slot *dev_registry_get_no_lock(struct dev_registry *reg,
						 dev_t dev)
{
	unsigned major = MAJOR(dev);
	struct dev_slot *slot;

	if (major >= BRK_MAJOR_MAX)
		return NULL;
	if (list_empty(&reg->lists[major]))
		return NULL;

	list_for_each_entry(slot, &reg->lists[major], list) {
		if (slot->dev == dev)
			return slot;
	}
	return NULL;
}

static void dev_registry_maybe_clear_pool(struct dev_registry *reg,
					  unsigned major)
{
	if (major >= BRK_MAJOR_MAX)
		return;
	if (list_empty(&reg->lists[major]))
		bitmap_clear_bit(reg->major_pooled, major);
}

int dev_registry_register(struct dev_registry *reg, struct dev_slot *slot,
			  dev_t dev, unsigned int dev_type)
{
	unsigned major = MAJOR(dev);
	unsigned minor = MINOR(dev);

	if (DEVTYPE(dev) != dev_type)
		return -EINVAL;
	if (major >= BRK_MAJOR_MAX || minor >= BRK_MINOR_MAX)
		return -EINVAL;

	spinlock_acquire(&reg->lock);
	if (dev_registry_get_no_lock(reg, dev) != NULL) {
		spinlock_release(&reg->lock);
		return -EBUSY;
	}
	slot->dev = dev;
	list_add(&slot->list, &reg->lists[major]);
	spinlock_release(&reg->lock);
	return 0;
}

void dev_registry_unregister(struct dev_registry *reg, struct dev_slot *slot)
{
	unsigned major = MAJOR(slot->dev);

	if (major >= BRK_MAJOR_MAX)
		return;

	spinlock_acquire(&reg->lock);
	if (dev_registry_get_no_lock(reg, slot->dev) != NULL)
		list_del(&slot->list);
	dev_registry_maybe_clear_pool(reg, major);
	spinlock_release(&reg->lock);
}

struct dev_slot *dev_registry_get(struct dev_registry *reg, dev_t dev)
{
	struct dev_slot *slot;

	spinlock_acquire(&reg->lock);
	slot = dev_registry_get_no_lock(reg, dev);
	spinlock_release(&reg->lock);
	return slot;
}

int dev_registry_alloc_major(struct dev_registry *reg, unsigned *major_out)
{
	unsigned m;

	spinlock_acquire(&reg->lock);
	for (m = BRK_DEV_FIRST_DYNAMIC_MAJOR; m < BRK_MAJOR_MAX; m++) {
		if (!bitmap_test_bit(reg->major_pooled, m) &&
		    list_empty(&reg->lists[m])) {
			bitmap_set_bit(reg->major_pooled, m);
			*major_out = m;
			spinlock_release(&reg->lock);
			return 0;
		}
	}
	spinlock_release(&reg->lock);
	return -ENOMEM;
}

void dev_registry_free_major(struct dev_registry *reg, unsigned major)
{
	if (major >= BRK_MAJOR_MAX)
		return;

	spinlock_acquire(&reg->lock);
	if (!list_empty(&reg->lists[major])) {
		spinlock_release(&reg->lock);
		return;
	}
	bitmap_clear_bit(reg->major_pooled, major);
	spinlock_release(&reg->lock);
}

int dev_registry_alloc_minor(struct dev_registry *reg, unsigned major,
			     unsigned *minor_out, unsigned int dev_type)
{
	unsigned n;

	if (major >= BRK_MAJOR_MAX || !minor_out)
		return -EINVAL;

	spinlock_acquire(&reg->lock);
	for (n = 0; n < BRK_DEV_ALLOC_MINOR_SCAN; n++) {
		dev_t dev = MKDEV(dev_type, major, n);
		if (!dev_registry_get_no_lock(reg, dev)) {
			*minor_out = n;
			spinlock_release(&reg->lock);
			return 0;
		}
	}
	spinlock_release(&reg->lock);
	return -ENOMEM;
}

int dev_registry_alloc_devnum(struct dev_registry *reg, dev_t *dev_out,
			      unsigned int dev_type)
{
	unsigned major, minor;

	if (!dev_out)
		return -EINVAL;

	spinlock_acquire(&reg->lock);
	for (major = BRK_DEV_FIRST_DYNAMIC_MAJOR; major < BRK_MAJOR_MAX;
	     major++) {
		for (minor = 0; minor < BRK_DEV_ALLOC_MINOR_SCAN; minor++) {
			dev_t dev = MKDEV(dev_type, major, minor);
			if (!dev_registry_get_no_lock(reg, dev)) {
				*dev_out = dev;
				spinlock_release(&reg->lock);
				return 0;
			}
		}
	}
	spinlock_release(&reg->lock);
	return -ENOMEM;
}
