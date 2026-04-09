#include "internal.h"
#include <brk/assert.h>
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/lock.h>
#include <brk/path.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_dir.h>
#include <ext4_fs.h>
#include <ext4_oflags.h>
#include <ext4_types.h>

int ext4fs_dir_open_entry(struct dentry *dir_dp, struct ext4_dir_en *en,
			  struct dentry *dp)
{
	struct inode *en_ip;
	struct ext4fs_inode *en_inode_info;
	int err;
	char *path;
	size_t en_name_len;
	struct ext4fs_super_block *sb_info;
	struct inode *dir_inode = dir_dp->d_inode;

	assert(sleeplock_holding(&dir_inode->i_lock));

	en_ip = inode_get(dir_inode->i_sb, en->inode);
	if (en_ip) {
		dp->d_inode = en_ip;
		dp->d_ops = &ext4_dops;
		return 0;
	}

	en_ip = inode_alloc();
	if (!en_ip)
		return -ENOMEM;

	en_inode_info = kzalloc(sizeof(*en_inode_info));
	if (!en_inode_info) {
		inode_free(en_ip);
		return -ENOMEM;
	}

	path = path_get_full(dir_dp);
	if (!path) {
		kfree(en_inode_info);
		inode_free(en_ip);
		return -ENOMEM;
	}

	sb_info = dir_inode->i_sb->s_private;
	en_name_len = ext4_dir_en_get_name_len(sb_info->s_sb, en);
	err = path_cat(path, (char *)en->name, en_name_len);
	if (err) {
		kfree(path);
		kfree(en_inode_info);
		inode_free(en_ip);
		return -ENOMEM;
	}

	if (en->in.inode_type == EXT4_DE_DIR) {
		en_inode_info->i_is_dir = true;
		err = ext4_dir_open(&en_inode_info->i_dir, path);
	} else {
		en_inode_info->i_is_dir = false;
		err = ext4_fopen2(&en_inode_info->i_file, path, O_RDWR);
	}
	if (err) {
		kfree(path);
		kfree(en_inode_info);
		inode_free(en_ip);
		return err;
	}

	en_ip->i_ops = &ext4_iops;
	en_ip->i_fops = &ext4_fops;
	sleeplock_acquire(&en_ip->i_lock);
	en_ip->i_private = en_inode_info;
	en_ip->i_no = en->inode;
	en_ip->i_sb = sblock_dup(dir_inode->i_sb);
	err = ext4fs_read_inode_metadata(en_ip);
	sleeplock_release(&en_ip->i_lock);
	if (err) {
		sblock_put(en_ip->i_sb);
		kfree(path);
		kfree(en_inode_info);
		inode_free(en_ip);
		return err;
	}

	struct inode *tmp = inode_get(dir_inode->i_sb, en->inode);
	if (tmp) {
		sblock_put(en_ip->i_sb);
		kfree(en_inode_info);
		inode_free(en_ip);
		dp->d_inode = tmp;
		dp->d_ops = &ext4_dops;
		kfree(path);
		return 0;
	}

	dp->d_inode = en_ip;
	dp->d_ops = &ext4_dops;

	inode_add(en_ip);

	kfree(path);

	return 0;
}

int ext4fs_dir_find_entry(struct dentry *dir, struct dentry *dentry)
{
	struct ext4fs_inode *dir_inode_info;
	struct ext4fs_super_block *sb_info;
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

	ret = ext4_fs_get_inode_ref(fs, dir_inode->i_no, &dir_inode_ref);
	if (ret != 0)
		goto out0;

	ret = ext4_dir_find_entry(&result, &dir_inode_ref, dentry->d_name,
				  strlen(dentry->d_name));
	if (ret != 0)
		goto out1;

	ret = ext4fs_dir_open_entry(dir, result.dentry, dentry);

	ext4_dir_destroy_result(&dir_inode_ref, &result);
out1:
	ext4_fs_put_inode_ref(&dir_inode_ref);
out0:
	return ret;
}

static int ext4fs_dentry_compare(struct dentry *dentry, const char *name,
				 size_t len)
{
	return strncmp(dentry->d_name, name, len);
}

const struct dentry_operations ext4_dops = {
	.compare = ext4fs_dentry_compare,
};
