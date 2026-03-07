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

int ext4fs_open_direntry(struct ext4_dir_en *dir_en, struct inode *dir_inode,
			 struct dentry *en_dentry)
{
	struct inode *en_inode = NULL;
	struct ext4fs_inode_info *en_inode_info = NULL;
	struct ext4_file *file = NULL;
	int err = 0;
	char *path = NULL;
	size_t en_name_len = 0;
	struct ext4fs_sb_info *sb_info;

	assert(sleeplock_holding(&dir_inode->i_lock));

	sb_info = dir_inode->i_sb->s_private;

	en_inode = inode_alloc();
	if (!en_inode) {
		err = -ENOMEM;
		goto err0;
	}

	en_inode_info = kzalloc(sizeof(*en_inode_info));
	if (!en_inode_info) {
		err = -ENOMEM;
		goto err1;
	}

	path = path_get_full(dir_inode->i_dentry);
	if (!path) {
		err = -ENOMEM;
		goto err2;
	}

	en_name_len = ext4_dir_en_get_name_len(sb_info->s_sb, dir_en);
	err = path_cat(path, (char *)dir_en->name, en_name_len);
	if (err)
		goto err3;

	if (dir_en->in.inode_type == EXT4_DE_DIR) {
		en_inode_info->i_is_dir = true;
		err = ext4_dir_open(&en_inode_info->i_dir, path);
		file = &en_inode_info->i_dir.f;
	} else {
		en_inode_info->i_is_dir = false;
		err = ext4_fopen2(&en_inode_info->i_file, path, O_RDWR);
		file = &en_inode_info->i_file;
	}
	if (err)
		goto err3;

	en_inode->i_ops = &ext4_iops;
	en_inode->i_fops = &ext4_fops;
	sleeplock_acquire(&en_inode->i_lock);
	en_inode->i_private = en_inode_info;
	en_inode->i_num = file->inode;
	en_inode->i_sb = sblock_dup(dir_inode->i_sb);
	err = ext4fs_read_inode(en_inode);
	sleeplock_release(&en_inode->i_lock);
	if (err) {
		sblock_put(en_inode->i_sb);
		goto err4;
	}

	en_dentry->d_inode = en_inode;
	en_inode->i_dentry = en_dentry;
	en_dentry->d_ops = &ext4_dops;

	inode_add(en_inode);

	kfree(path);

	return 0;

err4:
	if (en_inode_info->i_is_dir)
		ext4_dir_close(&en_inode_info->i_dir);
	else
		ext4_fclose(&en_inode_info->i_file);
err3:
	kfree(path);
err2:
	kfree(en_inode_info);
err1:
	inode_free(en_inode);
err0:
	return err;
}

static int ext4fs_dentry_compare(struct dentry *dentry, const char *name,
				 size_t len)
{
	return strncmp(dentry->d_name, name, len);
}

struct dentry_operations ext4_dops = {
	.compare = ext4fs_dentry_compare,
};
