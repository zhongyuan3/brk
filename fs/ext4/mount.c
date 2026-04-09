#include "internal.h"
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/lock.h>
#include <brk/path.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/types.h>
#include <ext4.h>
#include <ext4_super.h>
#include <ext4_types.h>

static SLEEPLOCK_DEFINE(__ext4fs_fs_lock);
static struct ext4_lock ext4fs_fs_lock;

static void ext4fs_fs_lock_acquire(void)
{
	sleeplock_acquire(&__ext4fs_fs_lock);
}

static void ext4fs_fs_lock_release(void)
{
	sleeplock_release(&__ext4fs_fs_lock);
}

static int ext4fs_mount(const struct file_system_type *fs_type,
			const char *dev_name, const char *mount_point,
			unsigned long flags, struct dentry **mount_root)
{
	struct dentry *dp;
	struct inode *ip;
	struct super_block *sb;
	struct blkdev *bdev;
	struct ext4fs_inode *efs_ip;
	struct ext4fs_super_block *efs_sb;
	struct ext4_sblock *e_sb;
	struct dentry *dev_dp;
	int err = 0;
	size_t dname_len = 0;
	const char *dname = NULL;

	sb = sblock_alloc();
	if (!sb)
		return -ENOMEM;

	sb->s_fs_type = &ext4_fs_type;
	sb->s_ops = &ext4_sops;

	dev_dp = path_lookup(dev_name);
	if (!dev_dp) {
		err = -ENODEV;
		goto err0;
	}
	if (!(inode_mode(dev_dp->d_inode) & S_IFBLK)) {
		dentry_put(dev_dp);
		err = -EINVAL;
		goto err0;
	}
	sb->s_dev = dev_dp->d_inode->i_rdev;
	dentry_put(dev_dp);
	bdev = blkdev_get(sb->s_dev);
	if (!bdev) {
		err = -ENODEV;
		goto err0;
	}

	efs_sb = ext4fs_alloc_sb(bdev);
	if (!efs_sb) {
		err = -ENOMEM;
		goto err0;
	}

	path_get_last(mount_point, &dname, &dname_len);
	dp = dentry_alloc(dname, dname_len);
	if (!dp) {
		err = -ENOMEM;
		goto err1;
	}

	ip = inode_alloc();
	if (!ip) {
		err = -ENOMEM;
		goto err2;
	}

	err = ext4_device_register(&efs_sb->s_blkdev, dev_name);
	if (err)
		goto err3;

	err = ext4_mount(dev_name, mount_point, false);
	if (err)
		goto err4;

	err = ext4_get_sblock(mount_point, &e_sb);
	if (err)
		goto err5;

	efs_sb->s_sb = e_sb;

	sb->s_block_size = ext4_sb_get_block_size(e_sb);
	sb->s_magic = e_sb->magic;
	sb->s_private = efs_sb;

	sb->s_root = dp;
	dp->d_parent = NULL;
	dp->d_ops = &ext4_dops;
	dp->d_flags |= DENTRY_MOUNTED;

	dp->d_inode = ip;

	efs_ip = kzalloc(sizeof(*efs_ip));
	if (!efs_ip) {
		err = -ENOMEM;
		goto err5;
	}

	err = ext4_dir_open(&efs_ip->i_dir, mount_point);
	if (err)
		goto err6;

	efs_ip->i_is_dir = true;
	ip->i_private = efs_ip;
	ip->i_no = efs_ip->i_dir.f.inode;
	ip->i_sb = sb;
	ip->i_ops = &ext4_iops;
	ip->i_fops = &ext4_fops;
	sleeplock_acquire(&ip->i_lock);
	err = ext4fs_read_inode_metadata(ip);
	sleeplock_release(&ip->i_lock);
	if (err)
		goto err7;

	sblock_add(sb);
	inode_add(ip);

	ext4_mount_setup_locks(mount_point, &ext4fs_fs_lock);

	*mount_root = dp;
	return 0;

err7:
	ext4_dir_close(&efs_ip->i_dir);
err6:
	kfree(efs_ip);
err5:
	ext4_umount(mount_point);
err4:
	ext4_device_unregister(dev_name);
err3:
	inode_free(ip);
err2:
	dentry_free(dp);
err1:
	ext4fs_free_sb(efs_sb);
err0:
	sblock_free(sb);
	return err;
}

static void ext4fs_umount(const char *mount_point)
{
}

static void ext4fs_deinit_sb(struct super_block *sb)
{
	struct ext4fs_super_block *sbi = sb->s_private;
	ext4fs_free_sb(sbi);
}

static struct ext4_lock ext4fs_fs_lock = {
	.lock = ext4fs_fs_lock_acquire,
	.unlock = ext4fs_fs_lock_release,
};

const struct file_system_type ext4_fs_type = {
	.name = "ext4",
	.deinit_sb = ext4fs_deinit_sb,
	.mount = ext4fs_mount,
	.umount = ext4fs_umount,
};
