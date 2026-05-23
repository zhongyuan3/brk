#include <brk/fs.h>
#include <brk/list.h>
#include <brk/string.h>

static LIST_DEFINE(filesystems);
static SPINLOCK_DEFINE(filesystems_lock);

int fs_driver_register(struct fs_driver *fs)
{
	spinlock_acquire(&filesystems_lock);
	list_add_tail(&fs->fs_list, &filesystems);
	spinlock_release(&filesystems_lock);
	return 0;
}

int fs_driver_unregister(struct fs_driver *fs)
{
	spinlock_acquire(&filesystems_lock);
	list_del_init(&fs->fs_list);
	spinlock_release(&filesystems_lock);
	return 0;
}

struct fs_driver *fs_driver_lookup(const char *name)
{
	struct fs_driver *fs;
	spinlock_acquire(&filesystems_lock);
	list_for_each_entry(fs, &filesystems, fs_list) {
		if (!strcmp(fs->name, name)) {
			spinlock_release(&filesystems_lock);
			return fs;
		}
	}
	spinlock_release(&filesystems_lock);
	return NULL;
}

void fs_driver_for_each(void (*fn)(const struct fs_driver *, void *), void *ctx)
{
	struct fs_driver *fs;

	if (!fn)
		return;
	spinlock_acquire(&filesystems_lock);
	list_for_each_entry(fs, &filesystems, fs_list)
		fn(fs, ctx);
	spinlock_release(&filesystems_lock);
}
