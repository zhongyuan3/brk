#include <brk/devnode.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/printk.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

struct devnode {
	struct list_head list;
	char path[64];
	dev_t dev;
	umode_t mode;
};

struct devnode_snapshot {
	char path[64];
	dev_t dev;
	umode_t mode;
};

static LIST_DEFINE(devnode_list);
static SPINLOCK_DEFINE(devnode_lock);

int devnode_register(const char *path, dev_t dev, umode_t mode)
{
	struct devnode *node;
	struct devnode *old;
	irqstate_t flags;

	if (!path || !path[0])
		return -EINVAL;

	node = kzalloc(sizeof(*node));
	if (!node)
		return -ENOMEM;

	strlcpy(node->path, path, sizeof(node->path));
	node->dev = dev;
	node->mode = mode;

	flags = spin_lock_irqsave(&devnode_lock);
	list_for_each_entry(old, &devnode_list, list) {
		if (!strcmp(old->path, path)) {
			/* Re-registration updates the existing entry. */
			old->dev = dev;
			old->mode = mode;
			spin_unlock_irqrestore(&devnode_lock, flags);
			kfree(node);
			return 0;
		}
	}
	list_add_tail(&node->list, &devnode_list);
	spin_unlock_irqrestore(&devnode_lock, flags);

	return 0;
}

void devnode_unregister(const char *path)
{
	struct devnode *node, *tmp;
	irqstate_t flags;

	if (!path)
		return;

	flags = spin_lock_irqsave(&devnode_lock);
	list_for_each_entry_safe(node, tmp, &devnode_list, list) {
		if (!strcmp(node->path, path)) {
			list_del(&node->list);
			kfree(node);
			break;
		}
	}
	spin_unlock_irqrestore(&devnode_lock, flags);
}

int devnode_populate(void)
{
	struct devnode_snapshot *snap;
	struct devnode *node;
	size_t count = 0;
	size_t i = 0;
	int first_err = 0;
	irqstate_t flags;

	flags = spin_lock_irqsave(&devnode_lock);
	list_for_each_entry(node, &devnode_list, list)
		count++;
	spin_unlock_irqrestore(&devnode_lock, flags);

	if (count == 0)
		return 0;

	snap = kcalloc(count, sizeof(*snap));
	if (!snap) {
		pr_warn("devnode: cannot allocate snapshot for %zu nodes\n",
			count);
		return -ENOMEM;
	}

	flags = spin_lock_irqsave(&devnode_lock);
	list_for_each_entry(node, &devnode_list, list) {
		if (i >= count)
			break;
		strlcpy(snap[i].path, node->path, sizeof(snap[i].path));
		snap[i].dev = node->dev;
		snap[i].mode = node->mode;
		i++;
	}
	spin_unlock_irqrestore(&devnode_lock, flags);

	/*
	 * Do the filesystem operations outside the registry lock; do_mknodat()
	 * may sleep.
	 */
	for (size_t j = 0; j < i; ++j) {
		int err = do_mknodat(AT_FDCWD, snap[j].path, snap[j].mode,
				     snap[j].dev);

		if (err && err != -EEXIST) {
			pr_warn("devnode: create %s failed: %d\n",
				snap[j].path, err);
			if (!first_err)
				first_err = err;
		}
	}

	kfree(snap);
	return first_err;
}
