#include <brk/fs.h>
#include <brk/list.h>
#include <brk/spinlock.h>
#include <brk/string.h>

static LIST_DEFINE(filesystems);
static SPINLOCK_DEFINE(filesystems_lock);

int fs_driver_register(struct fs_driver *fs)
{
	spin_lock(&filesystems_lock);
	list_add_tail(&fs->list, &filesystems);
	spin_unlock(&filesystems_lock);
	return 0;
}

int fs_driver_unregister(struct fs_driver *fs)
{
	spin_lock(&filesystems_lock);
	list_del_init(&fs->list);
	spin_unlock(&filesystems_lock);
	return 0;
}

struct fs_driver *fs_driver_lookup(const char *name)
{
	struct fs_driver *fs;
	spin_lock(&filesystems_lock);
	list_for_each_entry(fs, &filesystems, list) {
		if (!strcmp(fs->name, name)) {
			spin_unlock(&filesystems_lock);
			return fs;
		}
	}
	spin_unlock(&filesystems_lock);
	return NULL;
}

void fs_driver_for_each(void (*fn)(const struct fs_driver *, void *), void *ctx)
{
	struct fs_driver *fs;

	if (!fn)
		return;
	spin_lock(&filesystems_lock);
	list_for_each_entry(fs, &filesystems, list)
		fn(fs, ctx);
	spin_unlock(&filesystems_lock);
}
