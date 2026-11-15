#include <brk/fs.h>
#include <brk/list.h>
#include <brk/spinlock.h>
#include <brk/string.h>

static LIST_DEFINE(filesystems);
static SPINLOCK_DEFINE(filesystems_lock);

int file_system_type_register(struct file_system_type *type)
{
	spin_lock(&filesystems_lock);
	list_add_tail(&type->list, &filesystems);
	spin_unlock(&filesystems_lock);
	return 0;
}

int file_system_type_unregister(struct file_system_type *type)
{
	spin_lock(&filesystems_lock);
	list_del_init(&type->list);
	spin_unlock(&filesystems_lock);
	return 0;
}

struct file_system_type *file_system_type_lookup(const char *name)
{
	struct file_system_type *type;

	spin_lock(&filesystems_lock);
	list_for_each_entry(type, &filesystems, list) {
		if (!strcmp(type->name, name)) {
			spin_unlock(&filesystems_lock);
			return type;
		}
	}
	spin_unlock(&filesystems_lock);
	return NULL;
}

void file_system_type_for_each(void (*fn)(const struct file_system_type *,
					  void *),
			       void *ctx)
{
	struct file_system_type *type;

	if (!fn)
		return;
	spin_lock(&filesystems_lock);
	list_for_each_entry(type, &filesystems, list)
		fn(type, ctx);
	spin_unlock(&filesystems_lock);
}
