#include "internal.h"
#include <aosd/align.h>
#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/lock.h>
#include <aosd/path.h>
#include <aosd/string.h>
#include <aosd/types.h>
#include <ext4.h>
#include <ext4_oflags.h>
#include <ext4_types.h>
#include <uapi/aosd/dirent.h>
#include <uapi/aosd/stat.h>

static unsigned char ext4fs_get_dir_ent_type(const struct ext4_direntry *de)
{
	switch (de->inode_type) {
	case EXT4_DE_REG_FILE:
		return DT_REG;
	case EXT4_DE_DIR:
		return DT_DIR;
	case EXT4_DE_SYMLINK:
		return DT_LNK;
	case EXT4_DE_SOCK:
		return DT_SOCK;
	case EXT4_DE_FIFO:
		return DT_FIFO;
	case EXT4_DE_CHRDEV:
		return DT_CHR;
	case EXT4_DE_BLKDEV:
		return DT_BLK;
	default:
		return DT_UNKNOWN;
	}
}

static int ext4fs_read_dir(struct inode *ip, struct ext4_dir *dir, void *buf,
			   size_t size, off_t *offset, size_t *rcnt)
{
	const struct ext4_direntry *de = NULL;
	uint16_t reclen = 0;
	struct dirent64 *de64 = NULL;
	size_t r = 0;
	uint64_t p = (uint64_t)buf;
	struct ext4fs_inode *inode_info = ip->i_private;

	if ((uint64_t)*offset >= inode_info->i_dir_size)
		dir->next_off = (uint64_t)-1;
	else
		dir->next_off = *offset;

	while ((de = ext4_dir_entry_next(dir))) {
		reclen = DIRENT64_NAME_OFFSET + de->name_length + 1;
		reclen = align_up(reclen, alignof(de64[0]));
		if (r + reclen > size) {
			if (rcnt)
				*rcnt = r;
			return 0;
		}
		de64 = (struct dirent64 *)p;
		de64->d_ino = de->inode;
		de64->d_off = *offset;
		de64->d_reclen = reclen;
		de64->d_type = ext4fs_get_dir_ent_type(de);
		memcpy(de64->d_name, de->name, de->name_length);
		de64->d_name[de->name_length] = '\0';
		p += reclen;
		r += reclen;
		*offset += de->entry_length;
	}

	inode_info->i_dir_size = *offset;

	if (rcnt)
		*rcnt = r;

	return 0;
}

static int ext4fs_read_file(struct ext4_file *fp, void *buf, size_t buf_size,
			    off_t *offset, size_t *rcnt)
{
	int err;

	err = ext4_fseek(fp, *offset, SEEK_SET);
	if (err)
		return err;
	err = ext4_fread(fp, buf, buf_size, rcnt);
	if (err)
		return err;
	*offset = ext4_ftell(fp);
	return 0;
}

static int ext4fs_read(struct file *file, void *buf, size_t size, off_t *offset,
		       size_t *rcnt)
{
	struct ext4fs_inode *info;
	int ret = 0;
	struct inode *ip = file->f_inode;

	assert(file->f_inode);

	sleeplock_acquire(&ip->i_lock);

	info = ip->i_private;
	if (info->i_is_dir) {
		ret = ext4fs_read_dir(ip, &info->i_dir, buf, size, offset,
				      rcnt);
	} else {
		ret = ext4fs_read_file(&info->i_file, buf, size, offset, rcnt);
	}

	sleeplock_release(&ip->i_lock);
	return ret;
}

static int ext4fs_write(struct file *file, const void *buf, size_t size,
			off_t *offset, size_t *wcnt)
{
	struct ext4fs_inode *info;
	int ret = 0;

	sleeplock_acquire(&file->f_inode->i_lock);

	info = file->f_inode->i_private;
	if (info->i_is_dir) {
		ret = -EBADF;
		goto unlock_and_out;
	}

	ret = ext4_fseek(&info->i_file, *offset, SEEK_SET);
	if (ret != 0)
		goto unlock_and_out;
	ret = ext4_fwrite(&info->i_file, buf, size, wcnt);
	if (ret != 0)
		goto unlock_and_out;
	ret = ext4fs_read_inode_metadata(file->f_inode);
	if (ret != 0)
		goto unlock_and_out;
	*offset = ext4_ftell(&info->i_file);

unlock_and_out:
	sleeplock_release(&file->f_inode->i_lock);
	return ret;
}

static off_t ext4fs_seek(struct file *file, off_t offset, int whence)
{
	struct ext4fs_inode *info;
	off_t ret = 0;

	assert(file->f_inode);

	sleeplock_acquire(&file->f_inode->i_lock);

	info = file->f_inode->i_private;
	if (info->i_is_dir) {
		ret = -EISDIR;
		goto unlock_and_out;
	}

	ret = ext4_fseek(&info->i_file, offset, whence);
	if (ret == 0)
		ret = ext4_ftell(&info->i_file);

unlock_and_out:
	sleeplock_release(&file->f_inode->i_lock);
	return ret;
}

static int ext4fs_stat(struct file *file, struct stat *st)
{
	if (!file->f_inode)
		return -EBADF;

	memset(st, 0, sizeof(*st));

	sleeplock_acquire(&file->f_inode->i_lock);
	st->st_dev = file->f_inode->i_sb->s_dev;
	st->st_ino = file->f_inode->i_no;
	st->st_mode = file->f_inode->i_mode;
	st->st_nlink = file->f_inode->i_nlink;
	st->st_rdev = file->f_inode->i_rdev;
	st->st_size = file->f_inode->i_size;
	st->st_blksize = file->f_inode->i_sb->s_block_size;
	st->st_blocks = file->f_inode->i_size / st->st_blksize;
	sleeplock_release(&file->f_inode->i_lock);

	return 0;
}

static int ext4fs_open(struct file *file, struct dentry *dentry, int flags)
{
	struct ext4fs_inode *info;
	int ret = 0;
	struct inode *inode = dentry->d_inode;

	sleeplock_acquire(&inode->i_lock);

	info = inode->i_private;
	if (info->i_is_dir && flags != O_RDONLY) {
		ret = -EISDIR;
		goto unlock_and_out;
	}

	file->f_ops = &ext4_fops;
	file->f_inode = inode_dup(inode);
	file->f_dentry = dentry_dup(dentry);

unlock_and_out:
	sleeplock_release(&inode->i_lock);
	return ret;
}

static int ext4fs_truncate(struct file *file, off_t len)
{
	struct ext4fs_inode *info;
	int ret = 0;

	sleeplock_acquire(&file->f_inode->i_lock);

	info = file->f_inode->i_private;
	if (info->i_is_dir) {
		ret = -EISDIR;
		goto unlock_and_out;
	}

	ret = ext4_ftruncate(&info->i_file, len);

unlock_and_out:
	sleeplock_release(&file->f_inode->i_lock);
	return ret;
}

static int ext4fs_close(struct file *fp)
{
	return 0;
}

const struct file_operations ext4_fops = {
	.read = ext4fs_read,
	.write = ext4fs_write,
	.seek = ext4fs_seek,
	.stat = ext4fs_stat,
	.open = ext4fs_open,
	.truncate = ext4fs_truncate,
	.close = ext4fs_close,
};
