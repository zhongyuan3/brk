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
#include <ext4_dir.h>
#include <ext4_oflags.h>
#include <ext4_types.h>

int ext4fs_open_direntry(struct dentry *dir, struct ext4_dir_en *dir_en,
			 struct dentry *en_dentry)
{
	struct inode *en_inode;
	struct ext4fs_inode_info *en_inode_info;
	int err;
	char *path;
	size_t en_name_len;
	struct ext4fs_sb_info *sb_info;
	struct inode *dir_inode = dir->d_inode;

	assert(sleeplock_holding(&dir_inode->i_lock));

	en_inode = inode_get(dir_inode->i_sb, dir_en->inode);
	if (en_inode) {
		en_dentry->d_inode = en_inode;
		en_dentry->d_ops = &ext4_dops;
		return 0;
	}

	en_inode = inode_alloc();
	if (!en_inode)
		return -ENOMEM;

	en_inode_info = kzalloc(sizeof(*en_inode_info));
	if (!en_inode_info) {
		inode_free(en_inode);
		return -ENOMEM;
	}

	path = path_get_full(dir);
	if (!path) {
		kfree(en_inode_info);
		inode_free(en_inode);
		return -ENOMEM;
	}

	sb_info = dir_inode->i_sb->s_private;
	en_name_len = ext4_dir_en_get_name_len(sb_info->s_sb, dir_en);
	err = path_cat(path, (char *)dir_en->name, en_name_len);
	if (err) {
		kfree(path);
		kfree(en_inode_info);
		inode_free(en_inode);
		return -ENOMEM;
	}

	if (dir_en->in.inode_type == EXT4_DE_DIR) {
		en_inode_info->i_is_dir = true;
		err = ext4_dir_open(&en_inode_info->i_dir, path);
	} else {
		en_inode_info->i_is_dir = false;
		err = ext4_fopen2(&en_inode_info->i_file, path, O_RDWR);
	}
	if (err) {
		kfree(path);
		kfree(en_inode_info);
		inode_free(en_inode);
		return err;
	}

	en_inode->i_ops = &ext4_iops;
	en_inode->i_fops = &ext4_fops;
	sleeplock_acquire(&en_inode->i_lock);
	en_inode->i_private = en_inode_info;
	en_inode->i_num = dir_en->inode;
	en_inode->i_sb = sblock_dup(dir_inode->i_sb);
	err = ext4fs_read_inode(en_inode);
	sleeplock_release(&en_inode->i_lock);
	if (err) {
		sblock_put(en_inode->i_sb);
		kfree(path);
		kfree(en_inode_info);
		inode_free(en_inode);
		return err;
	}

	struct inode *tmp = inode_get(dir_inode->i_sb, dir_en->inode);
	if (tmp) {
		sblock_put(en_inode->i_sb);
		kfree(en_inode_info);
		inode_free(en_inode);
		en_dentry->d_inode = tmp;
		en_dentry->d_ops = &ext4_dops;
		kfree(path);
		return 0;
	}

	en_dentry->d_inode = en_inode;
	en_dentry->d_ops = &ext4_dops;

	inode_add(en_inode);

	kfree(path);

	return 0;
}

static int ext4fs_dentry_compare(struct dentry *dentry, const char *name,
				 size_t len)
{
	return strncmp(dentry->d_name, name, len);
}

struct dentry_operations ext4_dops = {
	.compare = ext4fs_dentry_compare,
};
