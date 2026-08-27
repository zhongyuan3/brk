#include "brkfs.h"
#include <arch/page.h>
#include <arch/pgtable.h>
#include <brk/base/error.h>
#include <brk/base/list.h>
#include <brk/base/types.h>
#include <brk/fs/dcache.h>
#include <brk/fs/fs.h>
#include <brk/fs/path.h>
#include <brk/lib/string.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/pagecache.h>
#include <brk/printk/printk.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/stat.h>

static int brkfs_read_super(struct brkfs_super_block *sb,
			    struct block_dev *bdev)
{
	struct cached_page *cp;
	uint8_t *buf;

	cp = read_mapping_page(bdev->bd_mapping, 0);
	if (IS_ERR(cp))
		return PTR_ERR(cp);

	buf = cached_page_addr(cp);
	memcpy(sb, buf + BRKFS_SUPER_OFFSET, sizeof(*sb));
	cached_page_put(cp);

	return 0;
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

	inode = path.dentry->inode;
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

static int brkfs_validate_super(struct brkfs_super_block *sb)
{
	if (sb->s_magic != BRKFS_MAGIC)
		return -EINVAL;

	if (sb->s_inode_size < sizeof(struct brkfs_inode))
		return -EINVAL;

	if (sb->s_blocksize > PAGE_SIZE || PAGE_SIZE % sb->s_blocksize != 0)
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
	if (err)
		return err;

	err = brkfs_validate_super(&brk_sb);
	if (err)
		return err;

	sb_info = brkfs_sb_info_alloc(bdev, &brk_sb);
	if (!sb_info)
		return -ENOMEM;

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

	spinlock_acquire(&args->driver->lock);
	list_add_tail(&sb->instance, &args->driver->super_blocks);
	spinlock_release(&args->driver->lock);

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
