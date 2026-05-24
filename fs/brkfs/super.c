#include "brkfs.h"
#include <brk/dcache.h>
#include <brk/fs.h>
#include <brk/pagecache.h>
#include <brk/slab.h>

static struct fs_inode *brkfs_alloc_inode(struct fs_super_block *sb)
{
	(void)sb;

	struct fs_inode *inode;
	struct brkfs_inode_info *info;

	inode = kzalloc(sizeof(*inode));
	if (!inode)
		return NULL;
	info = kzalloc(sizeof(*info));
	if (!info) {
		kfree(inode);
		return NULL;
	}
	inode->private_data = info;
	return inode;
}

static void brkfs_free_inode(struct fs_inode *inode)
{
	kfree(inode->private_data);
	inode->private_data = NULL;
	kfree(inode);
}

static void brkfs_dirty_inode(struct fs_inode *inode, int flags)
{
	(void)inode;
	(void)flags;
}

static int brkfs_write_inode(struct fs_inode *inode, int sync)
{
	struct brkfs_sb_info *sb_info = inode->sb->private_data;
	(void)sync;
	return brkfs_inode_write(sb_info, inode);
}

static void brkfs_evict_inode(struct fs_inode *inode)
{
	struct brkfs_sb_info *sbi = inode->sb->private_data;

	if (inode->nlink > 0) {
		/* Flush dirty cached data before the VFS releases the mapping.
		 * filemap_writeback is a no-op when ->i_mapping is NULL. */
		(void)filemap_writeback(inode->mapping);
		brkfs_inode_write(sbi, inode);
	} else {
		/* Drop the cache wholesale: pages would be invalid once the
		 * underlying disk blocks are freed below. */
		truncate_inode_pages(inode->mapping, 0);
		brkfs_truncate_inode_blocks(inode, 0);
		inode->size = 0;
		inode->nlink = 0;
		brkfs_inode_write(sbi, inode);
		brkfs_inode_free(sbi, (u32)inode->ino);
	}
}

static void brkfs_put_super(struct fs_super_block *sb)
{
	struct brkfs_sb_info *sb_info = sb->private_data;

	brkfs_sb_info_free(sb_info);
	sb->private_data = NULL;
	fs_dentry_put(sb->root);
	sb->root = NULL;
}

static int brkfs_sync_fs(struct fs_super_block *sb, int wait)
{
	(void)sb;
	(void)wait;
	return 0;
}

const struct fs_super_block_ops brkfs_sops = {
	.alloc_inode = brkfs_alloc_inode,
	.free_inode = brkfs_free_inode,
	.dirty_inode = brkfs_dirty_inode,
	.write_inode = brkfs_write_inode,
	.evict_inode = brkfs_evict_inode,
	.put_super = brkfs_put_super,
	.sync_fs = brkfs_sync_fs,
};
