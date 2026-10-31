#include "brkfs.h"
#include <arch/asm/page.h>
#include <arch/pgtable.h>
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/pagecache.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/stat.h>

static int brkfs_read_raw(struct block_dev *bdev, u64 off, void *buf,
			  size_t len)
{
	u8 *p = buf;

	while (len > 0) {
		pgoff_t index = (pgoff_t)(off / PAGE_SIZE);
		size_t in_page = (size_t)(off % PAGE_SIZE);
		size_t chunk = PAGE_SIZE - in_page;
		struct cached_page *cp;

		if (chunk > len)
			chunk = len;

		cp = read_mapping_page(bdev->bd_mapping, index);
		if (IS_ERR(cp))
			return PTR_ERR(cp);

		memcpy(p, cached_page_addr(cp) + in_page, chunk);
		cached_page_put(cp);

		p += chunk;
		off += chunk;
		len -= chunk;
	}

	return 0;
}

static bool brkfs_super_quick_ok(struct brkfs_super_block *sb)
{
	if (sb->s_magic != BRKFS_MAGIC)
		return false;
	if (sb->s_version != BRKFS_FORMAT_VERSION)
		return false;
	if (sb->s_checksum != brkfs_super_checksum(sb))
		return false;
	return true;
}

static int brkfs_read_super(struct brkfs_super_block *sb,
			    struct block_dev *bdev)
{
	int err;

	err = brkfs_read_raw(bdev, BRKFS_SUPER_OFFSET, sb, sizeof(*sb));
	if (err)
		return err;
	if (brkfs_super_quick_ok(sb))
		return 0;

	u64 capacity = blkdev_capacity_bytes(bdev);

	for (u32 bs = BRKFS_MIN_BLOCK_SIZE; bs <= BRKFS_MAX_BLOCK_SIZE;
	     bs *= 2) {
		struct brkfs_super_block backup;

		if (capacity < bs)
			continue;
		err = brkfs_read_raw(bdev, capacity - bs, &backup,
				     sizeof(backup));
		if (err)
			continue;
		if (!brkfs_super_quick_ok(&backup) ||
		    backup.s_blocksize != bs ||
		    backup.s_backup_super != backup.s_blocks_count - 1)
			continue;

		*sb = backup;
		return 0;
	}

	return -EINVAL;
}

static struct block_dev *brkfs_get_bdev(const char *dev_name)
{
	int err;
	struct fs_path path = { 0 };
	struct fs_inode *inode;
	struct block_dev *bdev;

	err = fs_path_lookup(dev_name, 0, &path);
	if (err)
		return ERR_PTR(err);

	/* A lookup may succeed for a negative dentry (missing device node). */
	inode = path.dentry ? path.dentry->inode : NULL;
	if (!inode) {
		fs_path_put(&path);
		return ERR_PTR(-ENOENT);
	}
	if (!S_ISBLK(inode->mode)) {
		fs_path_put(&path);
		return ERR_PTR(-ENOTBLK);
	}

	bdev = blkdev_get(inode->rdev);
	if (!bdev) {
		fs_path_put(&path);
		return ERR_PTR(-ENODEV);
	}

	fs_path_put(&path);
	return bdev;
}

static u64 brkfs_div_round_up(u64 num, u64 div)
{
	return (num + div - 1) / div;
}

static bool brkfs_is_pow_of_two(u32 value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static int brkfs_validate_super(struct brkfs_super_block *sb, u64 capacity)
{
	u64 bits_per_block;
	u64 inodes_per_block;
	u64 inode_bitmap_blocks;
	u64 data_bitmap_blocks;
	u64 inode_table_blocks;
	u64 volume_size;

	if (sb->s_magic != BRKFS_MAGIC)
		return -EINVAL;
	if (sb->s_version != BRKFS_FORMAT_VERSION)
		return -EINVAL;
	if (sb->s_features & ~BRKFS_KERNEL_SUPPORTED_FEATURES)
		return -EINVAL;

	if (sb->s_checksum != brkfs_super_checksum(sb))
		return -EINVAL;
	if (sb->s_state < BRKFS_STATE_CLEAN ||
	    sb->s_state > BRKFS_STATE_ERROR)
		return -EINVAL;
	if (sb->s_free_blocks > sb->s_data_blocks_count ||
	    sb->s_free_inodes > sb->s_inodes_count)
		return -EINVAL;
	if (sb->s_orphan > sb->s_inodes_count)
		return -EINVAL;

	if (sb->s_blocksize < BRKFS_MIN_BLOCK_SIZE ||
	    sb->s_blocksize > BRKFS_MAX_BLOCK_SIZE ||
	    !brkfs_is_pow_of_two(sb->s_blocksize))
		return -EINVAL;
	if (sb->s_inode_size < sizeof(struct brkfs_inode) ||
	    sb->s_inode_size > BRKFS_MAX_INODE_SIZE ||
	    !brkfs_is_pow_of_two(sb->s_inode_size) ||
	    sb->s_blocksize % sb->s_inode_size != 0)
		return -EINVAL;
	if (sb->s_blocks_count == 0 || sb->s_inodes_count == 0 ||
	    sb->s_data_blocks_count == 0)
		return -EINVAL;
	if (sb->s_blocks_count < 2 ||
	    sb->s_backup_super != sb->s_blocks_count - 1)
		return -EINVAL;

	volume_size = (u64)sb->s_blocks_count * sb->s_blocksize;
	if (capacity != 0 && volume_size > capacity)
		return -EINVAL;

	bits_per_block = (u64)sb->s_blocksize * 8;
	inodes_per_block = sb->s_blocksize / sb->s_inode_size;
	inode_bitmap_blocks =
		brkfs_div_round_up(sb->s_inodes_count, bits_per_block);
	data_bitmap_blocks =
		brkfs_div_round_up(sb->s_data_blocks_count, bits_per_block);
	inode_table_blocks =
		brkfs_div_round_up(sb->s_inodes_count, inodes_per_block);

	if (sb->s_inode_bitmap <
	    brkfs_div_round_up(BRKFS_SUPER_END_OFFSET, sb->s_blocksize))
		return -EINVAL;
	if ((u64)sb->s_inode_bitmap + inode_bitmap_blocks >
	    sb->s_data_block_bitmap)
		return -EINVAL;
	if ((u64)sb->s_data_block_bitmap + data_bitmap_blocks >
	    sb->s_inode_table)
		return -EINVAL;
	if ((u64)sb->s_inode_table + inode_table_blocks >
	    sb->s_first_data_block)
		return -EINVAL;
	if ((u64)sb->s_first_data_block + sb->s_data_blocks_count >
	    sb->s_backup_super)
		return -EINVAL;

	return 0;
}

int brkfs_mount(struct fs_mount_args *args, struct fs_mount_result *result)
{
	struct block_dev *bdev = NULL;
	int err;
	struct brkfs_super_block brk_sb;
	struct fs_super_block *sb;
	struct brkfs_sb_info *sb_info;
	struct fs_dentry *root_dentry;
	struct fs_inode *root_inode;

	bdev = brkfs_get_bdev(args->dev_name);
	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	err = brkfs_read_super(&brk_sb, bdev);
	if (err) {
		blkdev_put(bdev);
		return err;
	}

	err = brkfs_validate_super(&brk_sb, blkdev_capacity_bytes(bdev));
	if (err) {
		blkdev_put(bdev);
		return err;
	}

	/*
	 * brkfs_sb_info_alloc() takes ownership of the block-device
	 * reference obtained by brkfs_get_bdev(); brkfs_sb_info_free()
	 * releases it.
	 */
	sb_info = brkfs_sb_info_alloc(bdev, &brk_sb);
	if (!sb_info) {
		blkdev_put(bdev);
		return -ENOMEM;
	}

	if (sb_info->s_sb.s_state == BRKFS_STATE_ERROR) {
		brkfs_sb_info_free(sb_info);
		return -EIO;
	}
	if (sb_info->s_sb.s_state == BRKFS_STATE_DIRTY)
		pr_warn("brkfs: file system was not unmounted cleanly\n");

	brkfs_orphan_cleanup(sb_info);

	sb_info->s_sb.s_state = BRKFS_STATE_DIRTY;
	sb_info->s_sb.s_mnt_count++;
	sb_info->s_sb.s_mtime = (u32)ktime_get_real_sec();
	sb_info->s_super_dirty = true;
	err = brkfs_super_write(sb_info);
	if (err) {
		brkfs_sb_info_free(sb_info);
		return err;
	}

	sb = fs_super_block_alloc(args->driver);
	if (!sb) {
		brkfs_sb_info_free(sb_info);
		return -ENOMEM;
	}

	sb->block_size = brk_sb.s_blocksize;
	sb->magic = BRKFS_MAGIC;
	sb->flags = args->flags;
	sb->ops = &brkfs_sops;
	sb->default_dops = &generic_dop;

	sb->private_data = sb_info;

	root_inode = fs_inode_get_locked(sb, BRKFS_ROOT_INO);
	if (!root_inode) {
		fs_super_block_free(sb);
		brkfs_sb_info_free(sb_info);
		return -ENOMEM;
	}

	sleeplock_acquire(&root_inode->rwsem);
	err = brkfs_inode_read(sb_info, root_inode);
	sleeplock_release(&root_inode->rwsem);
	if (err) {
		fs_inode_fail_new(root_inode);
		fs_inode_put(root_inode);
		fs_super_block_free(sb);
		brkfs_sb_info_free(sb_info);
		return err;
	}
	brkfs_inode_setup_ops(root_inode);
	fs_inode_unlock_new(root_inode);

	root_dentry = fs_dentry_make_root(root_inode);
	if (!root_dentry) {
		fs_inode_put(root_inode);
		fs_super_block_free(sb);
		brkfs_sb_info_free(sb_info);
		return -ENOMEM;
	}

	sb->root = fs_dentry_get(root_dentry);

	spin_lock(&args->driver->lock);
	list_add_tail(&sb->instance, &args->driver->super_blocks);
	spin_unlock(&args->driver->lock);

	result->root = root_dentry;
	result->sb = sb;

	return 0;
}

struct fs_driver brkfs_fs_type = {
	.name = "brkfs",
	.mount = brkfs_mount,
	.lock = SPINLOCK_INITIALIZER("brkfs_fs_lock"),
	.super_blocks = LIST_INITIALIZER(brkfs_fs_type.super_blocks),
	.list = LIST_INITIALIZER(brkfs_fs_type.list),
};
