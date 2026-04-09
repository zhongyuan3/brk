#include "internal.h"
#include <brk/assert.h>
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/lock.h>
#include <brk/path.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/string.h>
#include <brk/types.h>
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_dir.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_oflags.h>
#include <ext4_types.h>

int ext4fs_read_inode_metadata(struct inode *ip)
{
	struct ext4fs_super_block *efs_sb;
	struct ext4fs_inode *efs_ip;
	struct ext4_fs *e_fs;
	struct ext4_inode_ref e_iref = { 0 };
	int err = 0;

	assert(sleeplock_holding(&ip->i_lock));

	efs_ip = ip->i_private;
	efs_sb = ip->i_sb->s_private;
	e_fs = efs_sb->s_blkdev.fs;

	err = ext4_fs_get_inode_ref(e_fs, ip->i_no, &e_iref);
	if (err)
		return err;

	ip->i_flags = ext4_inode_get_flags(e_iref.inode);
	ip->i_mode = ext4_inode_get_mode(&e_fs->sb, e_iref.inode);
	ip->i_nlink = ext4_inode_get_links_cnt(e_iref.inode);
	ip->i_size = ext4_inode_get_size(&e_fs->sb, e_iref.inode);
	ip->i_rdev = ext4_inode_get_dev(e_iref.inode);
	ip->i_atime.tv_sec = ext4_inode_get_access_time(e_iref.inode);
	ip->i_atime.tv_nsec = 0;
	ip->i_mtime.tv_sec = ext4_inode_get_modif_time(e_iref.inode);
	ip->i_mtime.tv_nsec = 0;
	ip->i_ctime.tv_sec = 0;
	ip->i_ctime.tv_nsec = 0;
	efs_ip->i_dir_size = ip->i_size;

	ext4_fs_put_inode_ref(&e_iref);

	return 0;
}

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

static int ext4fs_rename(struct dentry *old_dir, struct dentry *old_dp,
			 struct dentry *new_dir, struct dentry *new_dp)
{
	return 0;
}

static int ext4fs_symlink(struct dentry *dir_dp, struct dentry *dp,
			  const char *target)
{
	return 0;
}

static int ext4fs_readlink(struct dentry *dp, char *buf, size_t len)
{
	return 0;
}

const struct inode_operations ext4_iops = {
	.create = ext4fs_create,
	.link = ext4fs_link,
	.unlink = ext4fs_unlink,
	.mkdir = ext4fs_mkdir,
	.rmdir = ext4fs_rmdir,
	.lookup = ext4fs_lookup,
	.mknod = ext4fs_mknod,
	.rename = ext4fs_rename,
	.symlink = ext4fs_symlink,
	.readlink = ext4fs_readlink,
};
