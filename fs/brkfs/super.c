#include "brkfs.h"
#include <brk/dcache.h>
#include <brk/fs.h>
#include <brk/slab.h>

static struct inode *brkfs_alloc_inode(struct super_block *sb)
{
	struct inode *inode;
	struct brkfs_inode_info *info;

	inode = kzalloc(sizeof(*inode));
	if (!inode)
		return NULL;
	info = kzalloc(sizeof(*info));
	if (!info) {
		kfree(inode);
		return NULL;
	}
	inode->i_private = info;
	return inode;
}

static void brkfs_free_inode(struct inode *inode)
{
	kfree(inode->i_private);
	inode->i_private = NULL;
	kfree(inode);
}

static void brkfs_dirty_inode(struct inode *inode, int flags)
{
}

static int brkfs_write_inode(struct inode *inode, int sync)
{
	struct brkfs_sb_info *sb_info = inode->i_sb->s_fs_info;
	(void)sync;
	return brkfs_inode_write(sb_info, inode);
}

static void brkfs_evict_inode(struct inode *inode)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;

	if (inode->i_nlink > 0) {
		brkfs_inode_write(sbi, inode);
	} else {
		brkfs_truncate_inode_blocks(inode, 0);
		inode->i_size = 0;
		inode->i_nlink = 0;
		brkfs_inode_write(sbi, inode);
		brkfs_inode_free(sbi, (uint32_t)inode->i_ino);
	}
}

static void brkfs_put_super(struct super_block *sb)
{
	struct brkfs_sb_info *sb_info = sb->s_fs_info;

	brkfs_sb_info_free(sb_info);
	sb->s_fs_info = NULL;
	dentry_put(sb->s_root);
	sb->s_root = NULL;
}

static int brkfs_sync_fs(struct super_block *sb, int wait)
{
	(void)sb;
	(void)wait;
	return 0;
}

const struct super_operations brkfs_sops = {
	.alloc_inode = brkfs_alloc_inode,
	.free_inode = brkfs_free_inode,
	.dirty_inode = brkfs_dirty_inode,
	.write_inode = brkfs_write_inode,
	.evict_inode = brkfs_evict_inode,
	.put_super = brkfs_put_super,
	.sync_fs = brkfs_sync_fs,
};
