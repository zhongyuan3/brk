#include "internal.h"
#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/lock.h>
#include <aosd/path.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_dir.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_oflags.h>
#include <uapi/aosd/stat.h>

static int ext4fs_create(struct inode *dir_inode, struct dentry *new_dentry,
			 mode_t mode)
{
	char *path;
	struct inode *new_inode;
	struct ext4fs_inode_info *new_inode_info;
	int err;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir_inode->i_dentry);
	if (!path) {
		err = -ENOMEM;
		goto err0;
	}

	err = path_cat(path, new_dentry->d_name, strlen(new_dentry->d_name));
	if (err)
		goto err1;

	new_inode_info = kzalloc(sizeof(*new_inode_info));
	if (!new_inode_info) {
		err = -ENOMEM;
		goto err1;
	}

	err = ext4_fopen2(&new_inode_info->i_file, path, O_CREAT | O_EXCL);
	if (err)
		goto err2;

	new_inode = inode_alloc();
	if (!new_inode) {
		err = -ENOMEM;
		goto err3;
	}

	new_inode->i_ops = &ext4_iops;
	new_inode->i_fops = &ext4_fops;
	sleeplock_acquire(&new_inode->i_lock);
	new_inode->i_sb = sblock_dup(dir_inode->i_sb);
	new_inode->i_num = new_inode_info->i_file.inode;
	new_inode->i_private = new_inode_info;
	err = ext4fs_read_inode(new_inode);
	sleeplock_release(&new_inode->i_lock);
	if (err) {
		sblock_put(new_inode->i_sb);
		goto err4;
	}

	new_dentry->d_inode = new_inode;
	new_inode->i_dentry = new_dentry;
	new_dentry->d_ops = &ext4_dops;

	inode_add(new_inode);

	sleeplock_release(&dir_inode->i_lock);

	kfree(path);

	return 0;

err4:
	inode_free(new_inode);
err3:
	ext4_fclose(&new_inode_info->i_file);
err2:
	kfree(new_inode_info);
err1:
	kfree(path);
err0:
	sleeplock_release(&dir_inode->i_lock);
	return err;
}

static int ext4fs_link(struct dentry *old_dentry, struct inode *dir_inode,
		       struct dentry *new_dentry)
{
	char *old_path = NULL;
	char *new_path = NULL;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	old_path = path_get_full(old_dentry);
	if (!old_path) {
		ret = -ENOMEM;
		goto out0;
	}

	new_path = path_get_full(dir_inode->i_dentry);
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
		ret = ext4fs_dir_find_entry(dir_inode, new_dentry);

out2:
	kfree(new_path);
out1:
	kfree(old_path);
out0:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int ext4fs_unlink(struct inode *dir_inode, struct dentry *old_dentry)
{
	char *path = NULL;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir_inode->i_dentry);
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	ret = path_cat(path, old_dentry->d_name, strlen(old_dentry->d_name));
	if (ret != 0)
		goto out;

	ret = ext4_fremove(path);

out:
	sleeplock_release(&dir_inode->i_lock);
	kfree(path);
	return ret;
}

static int ext4fs_lookup(struct inode *dir_inode, struct dentry *dentry)
{
	int ret;

	sleeplock_acquire(&dir_inode->i_lock);
	ret = ext4fs_dir_find_entry(dir_inode, dentry);
	sleeplock_release(&dir_inode->i_lock);

	return ret;
}

static int ext4fs_mkdir(struct inode *dir_inode, struct dentry *new_dentry,
			mode_t mode)
{
	char *path = NULL;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir_inode->i_dentry);
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	ret = path_cat(path, new_dentry->d_name, strlen(new_dentry->d_name));
	if (ret != 0)
		goto out;

	ret = ext4_dir_mk(path);
	if (ret == 0)
		ret = ext4fs_dir_find_entry(dir_inode, new_dentry);

out:
	sleeplock_release(&dir_inode->i_lock);
	kfree(path);
	return ret;
}

static int ext4fs_rmdir(struct inode *dir_inode, struct dentry *old_dentry)
{
	char *path = NULL;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	path = path_get_full(dir_inode->i_dentry);
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	ret = path_cat(path, old_dentry->d_name, strlen(old_dentry->d_name));
	if (ret != 0)
		goto out;

	ret = ext4_dir_rm(path);

out:
	sleeplock_release(&dir_inode->i_lock);
	kfree(path);
	return ret;
}

static int ext4fs_mknod(struct inode *dir_inode, struct dentry *new_dentry,
			mode_t mode, dev_t dev)
{
	return -EOPNOTSUPP;
}

int ext4fs_dir_find_entry(struct inode *dir_inode, struct dentry *dentry)
{
	struct ext4fs_inode_info *dir_inode_info;
	struct ext4fs_sb_info *sb_info;
	struct ext4_fs *fs;
	struct ext4_inode_ref dir_inode_ref = { 0 };
	struct ext4_dir_search_result result = { 0 };
	int ret = 0;

	assert(sleeplock_holding(&dir_inode->i_lock));

	dir_inode_info = dir_inode->i_private;
	sb_info = dir_inode->i_sb->s_private;
	fs = sb_info->s_blkdev.fs;

	if (!dir_inode_info->i_is_dir) {
		ret = -EINVAL;
		goto out0;
	}

	ret = ext4_fs_get_inode_ref(fs, dir_inode->i_num, &dir_inode_ref);
	if (ret != 0)
		goto out0;

	ret = ext4_dir_find_entry(&result, &dir_inode_ref, dentry->d_name,
				  strlen(dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4fs_open_direntry(result.dentry, dir_inode, dentry);

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
	inode->i_dev = ext4_inode_get_dev(inode_ref.inode);
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
