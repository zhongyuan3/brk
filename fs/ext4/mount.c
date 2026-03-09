#include "internal.h"
#include <aosd/dcache.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/lock.h>
#include <aosd/path.h>
#include <aosd/slab.h>
#include <ext4.h>
#include <ext4_super.h>
#include <ext4_types.h>
#include <uapi/aosd/stat.h>

static struct dentry *ext4fs_alloc_root_dentry(const char *mount_point)
{
	const char *name = NULL;
	size_t len = 0;

	path_get_last(mount_point, &name, &len);
	return dentry_alloc(name, len);
}

static int ext4fs_mount(struct file_system_type *fs_type, const char *dev_name,
			const char *mount_point, unsigned long flags,
			struct dentry **mount_root)
{
	struct dentry *mnt_dentry;
	struct inode *mnt_inode;
	struct blkdev *blkdev;
	struct ext4fs_inode_info *inode_info;
	struct ext4fs_sb_info *sb_info;
	struct ext4_sblock *sb;
	struct dentry *dev_dentry;
	struct super_block *mnt_sb;
	int err = 0;

	mnt_sb = sblock_alloc();
	if (!mnt_sb)
		return -ENOMEM;

	mnt_sb->s_fs_type = &ext4_fs_type;
	mnt_sb->s_ops = &ext4_sops;

	dev_dentry = path_lookup(dev_name);
	if (!dev_dentry) {
		err = -ENODEV;
		goto err0;
	}
	if (!(inode_mode(dev_dentry->d_inode) & S_IFBLK)) {
		dentry_put(dev_dentry);
		err = -EINVAL;
		goto err0;
	}
	mnt_sb->s_dev = dev_dentry->d_inode->i_rdev;
	dentry_put(dev_dentry);
	blkdev = blkdev_get(mnt_sb->s_dev);
	if (!blkdev) {
		err = -ENODEV;
		goto err0;
	}

	sb_info = ext4fs_sbi_create(blkdev);
	if (!sb_info) {
		err = -ENOMEM;
		goto err0;
	}

	mnt_dentry = ext4fs_alloc_root_dentry(mount_point);
	if (!mnt_dentry) {
		err = -ENOMEM;
		goto err1;
	}

	mnt_inode = inode_alloc();
	if (!mnt_inode) {
		err = -ENOMEM;
		goto err2;
	}

	err = ext4_device_register(&sb_info->s_blkdev, dev_name);
	if (err)
		goto err3;

	err = ext4_mount(dev_name, mount_point, false);
	if (err)
		goto err4;

	err = ext4_get_sblock(mount_point, &sb);
	if (err)
		goto err5;

	sb_info->s_sb = sb;

	mnt_sb->s_block_size = ext4_sb_get_block_size(sb);
	mnt_sb->s_magic = sb->magic;
	mnt_sb->s_private = sb_info;

	mnt_sb->s_root = mnt_dentry;
	mnt_dentry->d_parent = NULL;
	mnt_dentry->d_ops = &ext4_dops;
	mnt_dentry->d_flags |= DENTRY_MOUNTED;

	mnt_dentry->d_inode = mnt_inode;

	inode_info = kzalloc(sizeof(*inode_info));
	if (!inode_info) {
		err = -ENOMEM;
		goto err5;
	}

	err = ext4_dir_open(&inode_info->i_dir, mount_point);
	if (err)
		goto err6;

	inode_info->i_is_dir = true;
	mnt_inode->i_private = inode_info;
	mnt_inode->i_num = inode_info->i_dir.f.inode;
	mnt_inode->i_sb = mnt_sb;
	mnt_inode->i_ops = &ext4_iops;
	mnt_inode->i_fops = &ext4_fops;
	sleeplock_acquire(&mnt_inode->i_lock);
	err = ext4fs_read_inode(mnt_inode);
	sleeplock_release(&mnt_inode->i_lock);
	if (err)
		goto err7;

	sblock_add(mnt_sb);
	inode_add(mnt_inode);

	*mount_root = mnt_dentry;
	return 0;

err7:
	ext4_dir_close(&inode_info->i_dir);
err6:
	kfree(inode_info);
err5:
	ext4_umount(mount_point);
err4:
	ext4_device_unregister(dev_name);
err3:
	inode_free(mnt_inode);
err2:
	dentry_free(mnt_dentry);
err1:
	ext4fs_sbi_destroy(sb_info);
err0:
	sblock_free(mnt_sb);
	return err;
}

static void ext4fs_umount(const char *mount_point)
{
}

static void ext4fs_deinit_sb(struct super_block *sb)
{
	struct ext4fs_sb_info *sbi = sb->s_private;
	ext4fs_sbi_destroy(sbi);
}

struct file_system_type ext4_fs_type = {
	.name = "ext4",
	.deinit_sb = ext4fs_deinit_sb,
	.mount = ext4fs_mount,
	.umount = ext4fs_umount,
};
