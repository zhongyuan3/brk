#include "brkfs.h"
#include <brk/dcache.h>
#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/path.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/string.h>

static int brkfs_read_super(struct brkfs_super_block *sb, struct blkdev *bdev)
{
	size_t bno = BRKFS_SUPER_OFFSET / bdev->phy_bsize;
	uint32_t cnt = BRKFS_SUPER_SIZE / bdev->phy_bsize;
	uint8_t *buf = kmalloc(sizeof(struct brkfs_super_block));
	int err;

	if (!buf)
		return -ENOMEM;
	err = bdev->ops->read(bdev, bno, buf, cnt);
	if (err) {
		kfree(buf);
		return err;
	}
	memcpy(sb, buf, sizeof(struct brkfs_super_block));
	kfree(buf);
	return 0;
}

static struct blkdev *brkfs_get_bdev(const char *dev_name)
{
	int err;
	struct path path = { 0 };
	struct inode *inode;
	struct blkdev *bdev;

	err = path_lookup(dev_name, 0, &path);
	if (err)
		return ERR_PTR(err);

	inode = path.dentry->d_inode;
	if (!S_ISBLK(inode->i_mode)) {
		path_put(&path);
		return ERR_PTR(-ENOTBLK);
	}

	bdev = blkdev_get(inode->i_rdev);
	if (!bdev) {
		path_put(&path);
		return ERR_PTR(-ENODEV);
	}

	path_put(&path);
	return bdev;
}

static int brkfs_validate_super(struct brkfs_super_block *sb)
{
	if (sb->s_magic != BRKFS_MAGIC)
		return -EINVAL;

	if (sb->s_inode_size != sizeof(struct brkfs_inode))
		return -EINVAL;

	log_info("block size: %u\n", sb->s_blocksize);
	log_info("inode size: %u\n", sb->s_inode_size);
	log_info("inode bitmap: %u\n", sb->s_inode_bitmap);
	log_info("inodes count: %u\n", sb->s_inodes_count);
	log_info("data block bitmap: %u\n", sb->s_data_block_bitmap);
	log_info("data blocks count: %u\n", sb->s_data_blocks_count);
	log_info("inode table: %u\n", sb->s_inode_table);
	log_info("first data block: %u\n", sb->s_first_data_block);
	log_info("magic: %u\n", sb->s_magic);
	log_info("blocks count: %u\n", sb->s_blocks_count);

	return 0;
}

struct dentry *brkfs_mount(struct file_system_type *fs_type, int flags,
			   const char *dev_name, void *data)
{
	struct blkdev *bdev = NULL;
	int err;
	struct brkfs_super_block brk_sb;
	struct super_block *sb;
	struct brkfs_sb_info *sb_info;
	struct dentry *root_dentry;
	struct inode *root_inode;

	(void)data;

	bdev = brkfs_get_bdev(dev_name);
	if (IS_ERR(bdev))
		return ERR_CAST(bdev);

	err = brkfs_read_super(&brk_sb, bdev);
	if (err)
		return ERR_PTR(err);

	err = brkfs_validate_super(&brk_sb);
	if (err)
		return ERR_PTR(err);

	sb_info = brkfs_sb_info_alloc(bdev, &brk_sb);
	if (!sb_info)
		return ERR_PTR(-ENOMEM);

	sb = alloc_super(fs_type);
	if (!sb) {
		brkfs_sb_info_free(sb_info);
		return ERR_PTR(-ENOMEM);
	}

	sb->s_blocksize = brk_sb.s_blocksize;
	sb->s_magic = BRKFS_MAGIC;
	sb->s_flags = flags;
	sb->s_op = &brkfs_sops;
	sb->s_d_op = &generic_dop;

	sb->s_fs_info = sb_info;

	root_inode = inode_get_locked(sb, BRKFS_ROOT_INO);
	if (!root_inode) {
		free_super(sb);
		brkfs_sb_info_free(sb_info);
		return ERR_PTR(-ENOMEM);
	}

	sleeplock_acquire(&root_inode->i_rwsem);
	err = brkfs_inode_read(sb_info, root_inode);
	sleeplock_release(&root_inode->i_rwsem);
	if (err) {
		inode_put(root_inode);
		free_super(sb);
		brkfs_sb_info_free(sb_info);
		return ERR_PTR(err);
	}
	brkfs_inode_setup_ops(root_inode);
	inode_unlock_new(root_inode);

	root_dentry = dentry_make_root(root_inode);
	if (!root_dentry) {
		inode_put(root_inode);
		free_super(sb);
		brkfs_sb_info_free(sb_info);
		return ERR_PTR(-ENOMEM);
	}

	sb->s_root = dentry_dup(root_dentry);

	spinlock_acquire(&fs_type->fs_lock);
	list_add_tail(&sb->s_instances, &fs_type->fs_supers);
	spinlock_release(&fs_type->fs_lock);

	return root_dentry;
}

static void brkfs_kill_sb(struct super_block *sb)
{
	struct file_system_type *fs_type = sb->s_type;

	spinlock_acquire(&fs_type->fs_lock);
	list_del(&sb->s_instances);
	spinlock_release(&fs_type->fs_lock);

	super_put(sb);
}

struct file_system_type brkfs_fs_type = {
	.name = "brkfs",
	.mount = brkfs_mount,
	.kill_sb = brkfs_kill_sb,
	.fs_lock = SPINLOCK_INITIALIZER("brkfs_fs_lock"),
	.fs_supers = LIST_INITIALIZER(brkfs_fs_type.fs_supers),
	.fs_list = LIST_INITIALIZER(brkfs_fs_type.fs_list),
};
