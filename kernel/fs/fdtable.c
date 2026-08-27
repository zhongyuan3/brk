#include <brk/base/error.h>
#include <brk/fs/fdtable.h>
#include <brk/fs/fs.h>
#include <brk/lib/refcnt.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <uapi/brk/errno.h>

struct file_desc_table *fdtable_alloc(void)
{
	struct file_desc_table *table;

	table = kzalloc(sizeof(struct file_desc_table));
	if (!table)
		return NULL;

	refcnt_init(&table->refcnt, 1);
	spinlock_init(&table->lock, "fdtable");

	return table;
}

struct file_desc_table *fdtable_get(struct file_desc_table *table)
{
	refcnt_inc(&table->refcnt);
	return table;
}

void fdtable_put(struct file_desc_table *table)
{
	if (refcnt_dec_fetch(&table->refcnt) > 0)
		return;

	/*
	 * refcnt dropped to zero: no other thread can reach this table,
	 * so no locking is needed while draining the file array.
	 */
	for (int i = 0; i < OPEN_MAX; i++) {
		if (table->files[i]) {
			fs_file_put(table->files[i]);
			table->files[i] = NULL;
		}
	}

	kfree(table);
}

void fdtable_copy(struct file_desc_table *dst, struct file_desc_table *src)
{
	for (int i = 0; i < OPEN_MAX; i++) {
		struct fs_file *old = NULL;
		struct fs_file *new_file = NULL;

		spinlock_acquire(&src->lock);
		if (src->files[i])
			new_file = fs_file_get(src->files[i]);
		spinlock_release(&src->lock);

		spinlock_acquire(&dst->lock);
		old = dst->files[i];
		dst->files[i] = new_file;
		spinlock_release(&dst->lock);

		if (old)
			fs_file_put(old);
	}
}

int fdtable_alloc_fd(struct file_desc_table *table, struct fs_file *file)
{
	int fd = -EMFILE;

	struct fs_file **files = table->files;
	spinlock_acquire(&table->lock);
	for (int i = 0; i < OPEN_MAX; i++) {
		if (!files[i]) {
			files[i] = file;
			fd = i;
			break;
		}
	}
	spinlock_release(&table->lock);
	return fd;
}

int fdtable_close_fd(struct file_desc_table *table, int fd)
{
	int ret = -EBADF;
	struct fs_file *file = NULL;
	struct fs_file **files = table->files;

	if (fd < 0 || fd >= OPEN_MAX)
		return -EBADF;

	spinlock_acquire(&table->lock);
	if (files[fd]) {
		file = files[fd];
		files[fd] = NULL;
		ret = 0;
	}
	spinlock_release(&table->lock);

	if (file)
		fs_file_put(file);
	return ret;
}

struct fs_file *fdtable_get_file(struct file_desc_table *table, int fd)
{
	if (fd < 0 || fd >= OPEN_MAX)
		return ERR_PTR(-EBADF);

	struct fs_file *file = NULL;
	struct fs_file **files = table->files;
	spinlock_acquire(&table->lock);
	if (files[fd])
		file = fs_file_get(files[fd]);
	spinlock_release(&table->lock);
	if (!file)
		return ERR_PTR(-EBADF);
	return file;
}

int fdtable_dup_fd(struct file_desc_table *table, int fd)
{
	struct fs_file **files = table->files;

	if (fd < 0 || fd >= OPEN_MAX)
		return -EBADF;

	spinlock_acquire(&table->lock);
	if (!files[fd]) {
		spinlock_release(&table->lock);
		return -EBADF;
	}
	for (int newfd = 0; newfd < OPEN_MAX; newfd++) {
		if (!files[newfd]) {
			files[newfd] = fs_file_get(files[fd]);
			spinlock_release(&table->lock);
			return newfd;
		}
	}
	spinlock_release(&table->lock);

	return -EMFILE;
}

int fdtable_dup_fd2(struct file_desc_table *table, int oldfd, int newfd)
{
	struct fs_file *old_file = NULL;
	struct fs_file **files = table->files;

	if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX)
		return -EBADF;

	spinlock_acquire(&table->lock);

	if (!files[oldfd]) {
		spinlock_release(&table->lock);
		return -EBADF;
	}

	if (files[newfd]) {
		if (files[newfd] != files[oldfd]) {
			old_file = files[newfd];
			files[newfd] = fs_file_get(files[oldfd]);
		}
	} else {
		files[newfd] = fs_file_get(files[oldfd]);
	}

	spinlock_release(&table->lock);

	if (old_file)
		fs_file_put(old_file);
	return 0;
}

void fdtable_close_all(struct file_desc_table *table)
{
	for (int i = 0; i < OPEN_MAX; i++) {
		struct fs_file *file = NULL;

		spinlock_acquire(&table->lock);
		file = table->files[i];
		table->files[i] = NULL;
		spinlock_release(&table->lock);

		if (file)
			fs_file_put(file);
	}
}
