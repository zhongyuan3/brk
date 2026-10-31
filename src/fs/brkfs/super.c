#include "brkfs.h"
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/pagecache.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <uapi/errno.h>

static u32 brkfs_feature_with_deps(u32 features)
{
	/*
	 * Triple-indirect trees pass through double-indirect levels, so the
	 * double bit must be set whenever the triple bit is used.
	 */
	if (features & BRKFS_FEATURE_TRIPLE_INDIRECT)
		features |= BRKFS_FEATURE_DOUBLE_INDIRECT;
	return features;
}

static int brkfs_write_bytes(struct block_dev *bdev, u64 off,
			     const void *buf, size_t len)
{
	const u8 *p = buf;

	while (len > 0) {
		pgoff_t index = (pgoff_t)(off / PAGE_SIZE);
		size_t in_page = (size_t)(off % PAGE_SIZE);
		size_t chunk = PAGE_SIZE - in_page;
		struct cached_page *cp;
		u8 *data;

		if (chunk > len)
			chunk = len;

		cp = read_mapping_page(bdev->bd_mapping, index);
		if (IS_ERR(cp))
			return PTR_ERR(cp);

		data = cached_page_addr(cp);
		cached_page_lock(cp);
		memcpy(data + in_page, p, chunk);
		cached_page_mark_dirty(cp);
		cached_page_unlock(cp);
		cached_page_put(cp);

		p += chunk;
		off += chunk;
		len -= chunk;
	}

	return 0;
}

int brkfs_super_write(struct brkfs_sb_info *sbi)
{
	struct brkfs_super_block sb;
	int err;

	spin_lock(&sbi->s_super_lock);
	sb = sbi->s_sb;
	spin_unlock(&sbi->s_super_lock);

	brkfs_super_set_checksum(&sb);

	err = brkfs_write_bytes(sbi->s_bdev, BRKFS_SUPER_OFFSET, &sb,
				sizeof(sb));
	if (err)
		return err;

	if (sb.s_backup_super == 0)
		return -EINVAL;

	u64 off = (u64)sb.s_backup_super * sb.s_blocksize;

	return brkfs_write_bytes(sbi->s_bdev, off, &sb, sizeof(sb));
}

int brkfs_super_set_features(struct brkfs_sb_info *sbi, u32 features)
{
	features = brkfs_feature_with_deps(features);

	spin_lock(&sbi->s_super_lock);
	if ((sbi->s_sb.s_features & features) == features) {
		spin_unlock(&sbi->s_super_lock);
		return 0;
	}
	sbi->s_sb.s_features |= features;
	sbi->s_super_dirty = true;
	spin_unlock(&sbi->s_super_lock);

	return brkfs_super_write(sbi);
}

struct brkfs_sb_info *brkfs_sb_info_alloc(struct block_dev *bdev,
					  struct brkfs_super_block *sb)
{
	struct brkfs_sb_info *sbi;

	sbi = kmalloc(sizeof(*sbi));
	if (!sbi)
		return NULL;

	sbi->s_bdev = bdev;
	sbi->s_sb = *sb;
	sbi->s_inodes_per_block = sb->s_blocksize / sb->s_inode_size;
	sbi->s_bits_per_block = sb->s_blocksize * 8;
	sbi->s_super_dirty = false;
	sleeplock_init(&sbi->s_orphan_lock, "brkfs_orphan");
	spinlock_init(&sbi->s_super_lock, "brkfs_super");

	return sbi;
}

void brkfs_sb_info_free(struct brkfs_sb_info *sbi)
{
	if (!sbi)
		return;
	if (sbi->s_bdev)
		blkdev_put(sbi->s_bdev);
	kfree(sbi);
}

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
		(void)page_cache_flush(inode->mapping);
		brkfs_inode_write(sbi, inode);
	} else {
		/* Drop the cache wholesale: pages would be invalid once the
		 * underlying disk blocks are freed below. */
		struct brkfs_inode_info *info = inode->private_data;

		(void)truncate_inode_pages(inode->mapping, 0);
		brkfs_orphan_remove(sbi, (u32)inode->ino);
		brkfs_truncate_inode_blocks(inode, 0);
		inode->size = 0;
		inode->nlink = 0;
		if (info)
			info->i_next_orphan = 0;
		brkfs_inode_write(sbi, inode);
		brkfs_inode_free(sbi, (u32)inode->ino);
	}
}

static int brkfs_sync_fs(struct fs_super_block *sb);

static void brkfs_put_super(struct fs_super_block *sb)
{
	struct brkfs_sb_info *sb_info = sb->private_data;
	struct fs_driver *driver = sb->driver;

	spin_lock(&driver->lock);
	list_del(&sb->instance);
	spin_unlock(&driver->lock);

	fs_dentry_put(sb->root);
	sb->root = NULL;

	if (sb_info) {
		int err = brkfs_sync_fs(sb);

		if (!err) {
			sb_info->s_sb.s_state = BRKFS_STATE_CLEAN;
			sb_info->s_super_dirty = true;
			(void)brkfs_sync_fs(sb);
		}
	}

	brkfs_sb_info_free(sb_info);
	sb->private_data = NULL;
}

static int brkfs_sync_fs(struct fs_super_block *sb)
{
	struct brkfs_sb_info *sbi = sb->private_data;
	int err;

	if (!sbi || !sbi->s_bdev || !sbi->s_bdev->bd_mapping)
		return 0;

	/*
	 * Inode tables, bitmaps and directory blocks live in the block
	 * device's page cache, not in any inode's mapping.  Without this
	 * flush they would stay dirty in memory forever.  Then ask the device
	 * to drain its volatile write cache.
	 */
	err = page_cache_flush(sbi->s_bdev->bd_mapping);

	if (!err && sbi->s_super_dirty)
		err = brkfs_super_write(sbi);

	if (!err)
		err = page_cache_flush(sbi->s_bdev->bd_mapping);

	if (!err)
		err = blkdev_flush(sbi->s_bdev);
	return err;
}

static int brkfs_statfs(struct fs_super_block *sb, struct statfs *buf)
{
	struct brkfs_sb_info *sbi = sb->private_data;
	u32 free_blocks;
	u32 free_inodes;

	spin_lock(&sbi->s_super_lock);
	free_blocks = sbi->s_sb.s_free_blocks;
	free_inodes = sbi->s_sb.s_free_inodes;
	spin_unlock(&sbi->s_super_lock);

	buf->f_type = BRKFS_MAGIC;
	buf->f_bsize = sbi->s_sb.s_blocksize;
	buf->f_frsize = sbi->s_sb.s_blocksize;
	buf->f_blocks = sbi->s_sb.s_data_blocks_count;
	buf->f_bfree = free_blocks;
	buf->f_bavail = free_blocks;
	buf->f_files = sbi->s_sb.s_inodes_count;
	buf->f_ffree = free_inodes;
	buf->f_namelen = BRKFS_NAME_LEN;
	return 0;
}

const struct fs_super_block_ops brkfs_sops = {
	.alloc_inode = brkfs_alloc_inode,
	.free_inode = brkfs_free_inode,
	.write_inode = brkfs_write_inode,
	.evict_inode = brkfs_evict_inode,
	.put_super = brkfs_put_super,
	.sync_fs = brkfs_sync_fs,
	.statfs = brkfs_statfs,
};
