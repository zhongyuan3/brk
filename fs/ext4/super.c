#include "internal.h"
#include <aosd/dcache.h>
#include <aosd/fs.h>
#include <aosd/lock.h>
#include <aosd/slab.h>
#include <ext4.h>
#include <ext4_blockdev.h>

static SLEEPLOCK_DEFINE(ext4fs_lock);

static void ext4fs_fs_lock(void)
{
	sleeplock_acquire(&ext4fs_lock);
}

static void ext4fs_fs_unlock(void)
{
	sleeplock_release(&ext4fs_lock);
}

static int ext4fs_bdev_open(struct ext4_blockdev *bdev)
{
	return 0;
}

static int ext4fs_bdev_bread(struct ext4_blockdev *bdev, void *buf,
			     uint64_t blk_id, uint32_t blk_cnt)
{
	struct blkdev *bd = bdev->bdif->p_user;
	return bd->ops->read(bd, blk_id, buf, blk_cnt);
}

static int ext4fs_bdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			      uint64_t blk_id, uint32_t blk_cnt)
{
	struct blkdev *bd = bdev->bdif->p_user;
	return bd->ops->write(bd, blk_id, buf, blk_cnt);
}

static int ext4fs_bdev_close(struct ext4_blockdev *bdev)
{
	return 0;
}

static int ext4fs_bdev_lock(struct ext4_blockdev *bdev)
{
	return 0;
}

static int ext4fs_bdev_unlock(struct ext4_blockdev *bdev)
{
	return 0;
}

static void ext4fs_deinit_inode(struct inode *inode)
{
	struct ext4fs_inode_info *info = inode->i_private;
	if (info->i_is_dir)
		ext4_dir_close(&info->i_dir);
	else
		ext4_fclose(&info->i_file);
	kfree(info);
	inode->i_private = NULL;
}

struct ext4fs_sb_info *ext4fs_sbi_create(struct blkdev *bd)
{
	struct ext4fs_sb_info *sbi;
	uint8_t *bbuf;
	struct ext4_blockdev_iface *bdif;

	bdif = kzalloc(sizeof(*bdif));
	if (!bdif)
		return NULL;

	bdif->open = ext4fs_bdev_open;
	bdif->bread = ext4fs_bdev_bread;
	bdif->bwrite = ext4fs_bdev_bwrite;
	bdif->close = ext4fs_bdev_close;
	bdif->lock = ext4fs_bdev_lock;
	bdif->unlock = ext4fs_bdev_unlock;
	bdif->ph_bsize = bd->phy_bsize;
	bdif->ph_bcnt = bd->phy_bcnt;
	bdif->p_user = bd;
	bbuf = kmalloc(bd->phy_bsize);
	if (!bbuf) {
		kfree(bdif);
		return NULL;
	}
	bdif->ph_bbuf = bbuf;

	sbi = kmalloc(sizeof(*sbi));
	if (!sbi) {
		kfree(bbuf);
		kfree(bdif);
		return NULL;
	}
	sbi->s_blkdev.bdif = bdif;
	sbi->s_blkdev.part_offset = 0;
	sbi->s_blkdev.part_size = bd->phy_bsize * bd->phy_bcnt;

	sbi->s_fs_lock.lock = ext4fs_fs_lock;
	sbi->s_fs_lock.unlock = ext4fs_fs_unlock;

	return sbi;
}

void ext4fs_sbi_destroy(struct ext4fs_sb_info *sbi)
{
	kfree(sbi->s_blkdev.bdif->ph_bbuf);
	kfree(sbi->s_blkdev.bdif);
	kfree(sbi);
}

struct super_operations ext4_sops = {
	.deinit_inode = ext4fs_deinit_inode,
};
