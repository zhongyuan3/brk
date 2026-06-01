#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
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

	struct fs_file **files = table->files;
	spinlock_acquire(&table->lock);
	for (int i = 0; i < OPEN_MAX; i++) {
		if (files[i]) {
			fs_file_put(files[i]);
			files[i] = NULL;
		}
	}
	spinlock_release(&table->lock);

	kfree(table);
}

void fdtable_copy(struct file_desc_table *dst, struct file_desc_table *src)
{
	struct fs_file **dst_files = dst->files;
	struct fs_file **src_files = src->files;
	spinlock_acquire(&src->lock);
	spinlock_acquire(&dst->lock);
	for (int i = 0; i < OPEN_MAX; i++) {
		if (dst_files[i])
			fs_file_put(dst_files[i]);
		if (src_files[i])
			dst_files[i] = fs_file_get(src_files[i]);
	}
	spinlock_release(&dst->lock);
	spinlock_release(&src->lock);
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
			fs_file_put(files[newfd]);
			files[newfd] = fs_file_get(files[oldfd]);
		}
	} else {
		files[newfd] = fs_file_get(files[oldfd]);
	}

	spinlock_release(&table->lock);
	return 0;
}

void fdtable_close_all(struct file_desc_table *table)
{
	struct fs_file **files = table->files;
	spinlock_acquire(&table->lock);
	for (int i = 0; i < OPEN_MAX; i++) {
		if (files[i]) {
			fs_file_put(files[i]);
			files[i] = NULL;
		}
	}
	spinlock_release(&table->lock);
}
