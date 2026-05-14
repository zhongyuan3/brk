#include <brk/fs.h>
#include <brk/list.h>
#include <brk/string.h>

static LIST_DEFINE(filesystems);
static SPINLOCK_DEFINE(filesystems_lock);

int register_filesystem(struct file_system_type *fs)
{
	spinlock_acquire(&filesystems_lock);
	list_add_tail(&fs->fs_list, &filesystems);
	spinlock_release(&filesystems_lock);
	return 0;
}

int unregister_filesystem(struct file_system_type *fs)
{
	spinlock_acquire(&filesystems_lock);
	list_del_init(&fs->fs_list);
	spinlock_release(&filesystems_lock);
	return 0;
}

struct file_system_type *get_filesystem(const char *name)
{
	struct file_system_type *fs;
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

void for_each_filesystem(void (*fn)(const struct file_system_type *, void *),
			 void *ctx)
{
	struct file_system_type *fs;

	if (!fn)
		return;
	spinlock_acquire(&filesystems_lock);
	list_for_each_entry(fs, &filesystems, fs_list)
		fn(fs, ctx);
	spinlock_release(&filesystems_lock);
}
