#include "internal.h"
#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/lock.h>
#include <aosd/path.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/types.h>
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_dir.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_oflags.h>
#include <uapi/aosd/stat.h>

static int ext4fs_create(struct dentry *dir, struct dentry *new_dentry,
			 mode_t mode)
{
	char *path;
	struct ext4_file file;
	int ret;
	struct inode *dir_inode = dir->d_inode;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir);
	if (!path) {
		ret = -ENOMEM;
		goto out0;
	}

	ret = path_cat(path, new_dentry->d_name, strlen(new_dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4_fopen2(&file, path, O_CREAT);
	if (ret != 0)
		goto out1;
	ext4_fclose(&file);

	ret = ext4fs_dir_find_entry(dir, new_dentry);

out1:
	kfree(path);
out0:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int ext4fs_link(struct dentry *old_dentry, struct dentry *dir,
		       struct dentry *new_dentry)
{
	char *old_path;
	char *new_path;
	int ret;
	struct inode *dir_inode = dir->d_inode;

	sleeplock_acquire(&dir_inode->i_lock);

	old_path = path_get_full(old_dentry);
	if (!old_path) {
		ret = -ENOMEM;
		goto out0;
	}

	new_path = path_get_full(dir);
	if (!new_path) {
		ret = -ENOMEM;
		goto out1;
	}

	ret = path_cat(new_path, new_dentry->d_name,
		       strlen(new_dentry->d_name));
	if (ret != 0)
		goto out2;

	ret = ext4_flink(old_path, new_path);
	if (ret == 0)
		ret = ext4fs_dir_find_entry(dir, new_dentry);

out2:
	kfree(new_path);
out1:
	kfree(old_path);
out0:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int ext4fs_unlink(struct dentry *dir, struct dentry *old_dentry)
{
	char *path;
	int ret;
	struct inode *dir_inode = dir->d_inode;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir);
	if (!path) {
		ret = -ENOMEM;
		goto out0;
	}

	ret = path_cat(path, old_dentry->d_name, strlen(old_dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4_fremove(path);

out1:
	kfree(path);
out0:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int ext4fs_lookup(struct dentry *dir, struct dentry *dentry)
{
	int ret;
	struct inode *dir_inode = dir->d_inode;
	sleeplock_acquire(&dir_inode->i_lock);
	ret = ext4fs_dir_find_entry(dir, dentry);
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int ext4fs_mkdir(struct dentry *dir, struct dentry *new_dentry,
			mode_t mode)
{
	char *path;
	int ret;
	struct inode *dir_inode = dir->d_inode;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir);
	if (!path) {
		ret = -ENOMEM;
		goto out0;
	}

	ret = path_cat(path, new_dentry->d_name, strlen(new_dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4_dir_mk(path);
	if (ret == 0)
		ret = ext4fs_dir_find_entry(dir, new_dentry);

out1:
	kfree(path);
out0:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int ext4fs_rmdir(struct dentry *dir, struct dentry *old_dentry)
{
	char *path;
	int ret;
	struct inode *dir_inode = dir->d_inode;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir);
	if (!path) {
		ret = -ENOMEM;
		goto out0;
	}

	ret = path_cat(path, old_dentry->d_name, strlen(old_dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4_dir_rm(path);

out1:
	kfree(path);
out0:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int ext4fs_mode_to_filetype(mode_t mode)
{
	if (mode & S_IFSOCK)
		return EXT4_DE_SOCK;
	else if (mode & S_IFLNK)
		return EXT4_DE_SYMLINK;
	else if (mode & S_IFREG)
		return EXT4_DE_REG_FILE;
	else if (mode & S_IFBLK)
		return EXT4_DE_BLKDEV;
	else if (mode & S_IFDIR)
		return EXT4_DE_DIR;
	else if (mode & S_IFCHR)
		return EXT4_DE_CHRDEV;
	else if (mode & S_IFIFO)
		return EXT4_DE_FIFO;
	else
		return -EINVAL;
}

static int ext4fs_mknod(struct dentry *dir, struct dentry *new_dentry,
			mode_t mode, dev_t dev)
{
	char *path;
	int ret;
	int filetype;
	struct inode *dir_inode = dir->d_inode;

	filetype = ext4fs_mode_to_filetype(mode);
	if (filetype < 0)
		return -EINVAL;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir);
	if (!path) {
		ret = -ENOMEM;
		goto out0;
	}

	ret = path_cat(path, new_dentry->d_name, strlen(new_dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4_mknod(path, filetype, dev);
	if (ret == 0)
		ret = ext4fs_dir_find_entry(dir, new_dentry);

out1:
	kfree(path);
out0:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

int ext4fs_dir_find_entry(struct dentry *dir, struct dentry *dentry)
{
	struct ext4fs_inode_info *dir_inode_info;
	struct ext4fs_sb_info *sb_info;
	struct ext4_fs *fs;
	struct ext4_inode_ref dir_inode_ref = { 0 };
	struct ext4_dir_search_result result = { 0 };
	int ret = 0;
	struct inode *dir_inode = dir->d_inode;

	assert(sleeplock_holding(&dir_inode->i_lock));

	dir_inode_info = dir_inode->i_private;
	sb_info = dir_inode->i_sb->s_private;
	fs = sb_info->s_blkdev.fs;

	if (!dir_inode_info->i_is_dir) {
		ret = -ENOTDIR;
		goto out0;
	}

	ret = ext4_fs_get_inode_ref(fs, dir_inode->i_num, &dir_inode_ref);
	if (ret != 0)
		goto out0;

	ret = ext4_dir_find_entry(&result, &dir_inode_ref, dentry->d_name,
				  strlen(dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4fs_open_direntry(dir, result.dentry, dentry);

	ext4_dir_destroy_result(&dir_inode_ref, &result);
out1:
	ext4_fs_put_inode_ref(&dir_inode_ref);
out0:
	return ret;
}

int ext4fs_read_inode(struct inode *inode)
{
	struct ext4fs_sb_info *sb_info;
	struct ext4_fs *fs;
	struct ext4_inode_ref inode_ref = { 0 };
	struct ext4fs_inode_info *inode_info;

	int err = 0;

	assert(sleeplock_holding(&inode->i_lock));

	inode_info = inode->i_private;
	sb_info = inode->i_sb->s_private;
	fs = sb_info->s_blkdev.fs;

	err = ext4_fs_get_inode_ref(fs, inode->i_num, &inode_ref);
	if (err)
		return err;

	inode->i_flags = ext4_inode_get_flags(inode_ref.inode);
	inode->i_mode = ext4_inode_get_mode(&fs->sb, inode_ref.inode);
	inode->i_links = ext4_inode_get_links_cnt(inode_ref.inode);
	inode->i_size = ext4_inode_get_size(&fs->sb, inode_ref.inode);
	inode->i_rdev = ext4_inode_get_dev(inode_ref.inode);
	inode->i_atime = ext4_inode_get_access_time(inode_ref.inode);
	inode->i_mtime = ext4_inode_get_modif_time(inode_ref.inode);
	inode->i_ctime = ext4_inode_get_change_inode_time(inode_ref.inode);
	inode_info->i_dir_size = inode->i_size;

	ext4_fs_put_inode_ref(&inode_ref);

	return 0;
}

struct inode_operations ext4_iops = {
	.create = ext4fs_create,
	.link = ext4fs_link,
	.unlink = ext4fs_unlink,
	.mkdir = ext4fs_mkdir,
	.rmdir = ext4fs_rmdir,
	.lookup = ext4fs_lookup,
	.mknod = ext4fs_mknod,
};
