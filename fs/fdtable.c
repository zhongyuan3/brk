#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <uapi/errno.h>

void fdtable_init(struct file_desc_table *table)
{
	memset(table, 0, sizeof(*table));
	spinlock_init(&table->lock, "fdtable");
}

void fdtable_deinit(struct file_desc_table *table)
{
	for (int i = 0; i < OPEN_MAX; i++) {
		struct file *file = NULL;

		spin_lock(&table->lock);
		file = table->files[i];
		table->files[i] = NULL;
		spin_unlock(&table->lock);

		if (file)
			file_put(file);
	}
}

void fdtable_copy(struct file_desc_table *dst, struct file_desc_table *src)
{
	for (int i = 0; i < OPEN_MAX; i++) {
		struct file *old = NULL;
		struct file *new_file = NULL;

		spin_lock(&src->lock);
		if (src->files[i])
			new_file = file_get(src->files[i]);
		spin_unlock(&src->lock);

		spin_lock(&dst->lock);
		old = dst->files[i];
		dst->files[i] = new_file;
		spin_unlock(&dst->lock);

		if (old)
			file_put(old);
	}
}

int fdtable_alloc_fd(struct file_desc_table *table, struct file *file)
{
	int fd = -EMFILE;

	struct file **files = table->files;
	spin_lock(&table->lock);
	for (int i = 0; i < OPEN_MAX; i++) {
		if (!files[i]) {
			files[i] = file;
			fd = i;
			break;
		}
	}
	spin_unlock(&table->lock);
	return fd;
}

int fdtable_close_fd(struct file_desc_table *table, int fd)
{
	int ret = -EBADF;
	struct file *file = NULL;
	struct file **files = table->files;

	if (fd < 0 || fd >= OPEN_MAX)
		return -EBADF;

	spin_lock(&table->lock);
	if (files[fd]) {
		file = files[fd];
		files[fd] = NULL;
		ret = 0;
	}
	spin_unlock(&table->lock);

	if (file)
		file_put(file);
	return ret;
}

struct file *fdtable_get_file(struct file_desc_table *table, int fd)
{
	if (fd < 0 || fd >= OPEN_MAX)
		return ERR_PTR(-EBADF);

	struct file *file = NULL;
	struct file **files = table->files;
	spin_lock(&table->lock);
	if (files[fd])
		file = file_get(files[fd]);
	spin_unlock(&table->lock);
	if (!file)
		return ERR_PTR(-EBADF);
	return file;
}

int fdtable_dup_fd(struct file_desc_table *table, int fd)
{
	struct file **files = table->files;

	if (fd < 0 || fd >= OPEN_MAX)
		return -EBADF;

	spin_lock(&table->lock);
	if (!files[fd]) {
		spin_unlock(&table->lock);
		return -EBADF;
	}
	for (int newfd = 0; newfd < OPEN_MAX; newfd++) {
		if (!files[newfd]) {
			files[newfd] = file_get(files[fd]);
			spin_unlock(&table->lock);
			return newfd;
		}
	}
	spin_unlock(&table->lock);

	return -EMFILE;
}

int fdtable_dup_fd2(struct file_desc_table *table, int oldfd, int newfd)
{
	struct file *old_file = NULL;
	struct file **files = table->files;

	if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX)
		return -EBADF;

	spin_lock(&table->lock);

	if (!files[oldfd]) {
		spin_unlock(&table->lock);
		return -EBADF;
	}

	if (files[newfd]) {
		if (files[newfd] != files[oldfd]) {
			old_file = files[newfd];
			files[newfd] = file_get(files[oldfd]);
		}
	} else {
		files[newfd] = file_get(files[oldfd]);
	}

	spin_unlock(&table->lock);

	if (old_file)
		file_put(old_file);
	return 0;
}
