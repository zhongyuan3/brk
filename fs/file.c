#include <aosd/align.h>
#include <aosd/assert.h>
#include <aosd/dev.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/pipe.h>
#include <aosd/slab.h>
#include <aosd/string.h>

static struct kmem_cache fcache;
static list_head_define(flist);
static spinlock_define(flist_lock);

int file_cache_init(void)
{
	return kmem_cache_init(&fcache, sizeof(struct file),
			       alignof(struct file), "fcache");
}

static struct file *__file_alloc(void)
{
	struct file *fp;

	fp = kmem_cache_alloc(&fcache);
	if (fp)
		memset(fp, 0, sizeof(*fp));
	return fp;
}

static void __file_free(struct file *fp)
{
	kmem_cache_free(&fcache, fp);
}

struct file *file_alloc(void)
{
	struct file *fp;

	fp = __file_alloc();
	if (!fp)
		return NULL;

	list_init_head(&fp->f_list);
	fp->f_rc = 1;

	spinlock_acquire(&flist_lock);
	list_add(&fp->f_list, &flist);
	spinlock_release(&flist_lock);

	return fp;
}

void file_put(struct file *fp)
{
	assert(fp);
	assert(fp->f_rc > 0);

	spinlock_acquire(&flist_lock);
	--fp->f_rc;
	if (fp->f_rc <= 0) {
		list_del(&fp->f_list);
		spinlock_release(&flist_lock);
		if (fp->f_pipe)
			pipe_close(fp->f_pipe, fp->f_mode & FMODE_WRITE);
		if (fp->f_inode)
			inode_put(fp->f_inode);
		__file_free(fp);
	} else {
		spinlock_release(&flist_lock);
	}
}

struct file *file_dup(struct file *fp)
{
	assert(fp);
	assert(fp->f_rc > 0);
	spinlock_acquire(&flist_lock);
	++fp->f_rc;
	spinlock_release(&flist_lock);
	return fp;
}

int file_read(struct file *fp, void *buf, size_t cnt, size_t *rcnt)
{
	if (!(fp->f_mode & FMODE_READ))
		return -EBADF;

	return fp->f_ops->read(fp, buf, cnt, &fp->f_off, rcnt);
}

int file_write(struct file *fp, const void *buf, size_t cnt, size_t *wcnt)
{
	if (!(fp->f_mode & FMODE_WRITE))
		return -EBADF;

	return fp->f_ops->write(fp, buf, cnt, &fp->f_off, wcnt);
}

off_t file_seek(struct file *fp, off_t offset, int whence)
{
	off_t ret = fp->f_ops->seek(fp, offset, whence);
	if (ret >= 0)
		fp->f_off = ret;
	return ret;
}

int file_stat(struct file *fp, struct stat *buf)
{
	return fp->f_ops->stat(fp, buf);
}

int file_truncate(struct file *fp, off_t len)
{
	return fp->f_ops->truncate(fp, len);
}
