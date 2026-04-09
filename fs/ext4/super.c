#include "internal.h"
#include <brk/dcache.h>
#include <brk/fs.h>
#include <brk/slab.h>
#include <brk/types.h>
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_dir.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_types.h>

static int ext4fs_bdev_open(struct ext4_blockdev *bdev)
{
	return 0;
}

static int ext4fs_bdev_read(struct ext4_blockdev *bdev, void *buf,
			    uint64_t blk_id, uint32_t blk_cnt)
{
	struct blkdev *bd = bdev->bdif->p_user;
	return bd->ops->read(bd, blk_id, buf, blk_cnt);
}

static int ext4fs_bdev_write(struct ext4_blockdev *bdev, const void *buf,
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

struct ext4fs_super_block *ext4fs_alloc_sb(struct blkdev *bd)
{
	struct ext4fs_super_block *sb;
	uint8_t *bdbuf;
	struct ext4_blockdev_iface *bdif;

	bdif = kzalloc(sizeof(*bdif));
	if (!bdif)
		return NULL;

	bdif->open = ext4fs_bdev_open;
	bdif->bread = ext4fs_bdev_read;
	bdif->bwrite = ext4fs_bdev_write;
	bdif->close = ext4fs_bdev_close;
	bdif->lock = ext4fs_bdev_lock;
	bdif->unlock = ext4fs_bdev_unlock;
	bdif->ph_bsize = bd->phy_bsize;
	bdif->ph_bcnt = bd->phy_bcnt;
	bdif->p_user = bd;
	bdbuf = kmalloc(bd->phy_bsize);
	if (!bdbuf) {
		kfree(bdif);
		return NULL;
	}
	bdif->ph_bbuf = bdbuf;

	sb = kzalloc(sizeof(*sb));
	if (!sb) {
		kfree(bdbuf);
		kfree(bdif);
		return NULL;
	}
	sb->s_blkdev.bdif = bdif;
	sb->s_blkdev.part_offset = 0;
	sb->s_blkdev.part_size = bd->phy_bsize * bd->phy_bcnt;

	return sb;
}

void ext4fs_free_sb(struct ext4fs_super_block *sb)
{
	kfree(sb->s_blkdev.bdif->ph_bbuf);
	kfree(sb->s_blkdev.bdif);
	kfree(sb);
}

static void ext4fs_deinit_inode(struct inode *inode)
{
	struct ext4fs_inode *info = inode->i_private;
	if (info->i_is_dir)
		ext4_dir_close(&info->i_dir);
	else
		ext4_fclose(&info->i_file);
	kfree(info);
	inode->i_private = NULL;
}

static int ext4fs_read_inode(struct inode *ip, void *priv)
{
	struct ext4_inode_ref iref;
	struct ext4_fs *fs = NULL;
	struct ext4_sblock *sb = NULL;
	int err;

	err = ext4_fs_get_inode_ref(fs, ip->i_no, &iref);
	if (err)
		return err;

	ip->i_rdev = ext4_inode_get_dev(iref.inode);
	ip->i_mode = ext4_inode_get_mode(sb, iref.inode);
	ip->i_nlink = ext4_inode_get_links_cnt(iref.inode);
	ip->i_size = ext4_inode_get_size(sb, iref.inode);
	ip->i_uid = ext4_inode_get_uid(iref.inode);
	ip->i_gid = ext4_inode_get_gid(iref.inode);
	ip->i_atime.tv_sec = ext4_inode_get_access_time(iref.inode);
	ip->i_atime.tv_nsec = 0;
	ip->i_mtime.tv_sec = ext4_inode_get_modif_time(iref.inode);
	ip->i_mtime.tv_nsec = 0;
	ip->i_ctime.tv_sec = 0;
	ip->i_ctime.tv_nsec = 0;

	return ext4_fs_put_inode_ref(&iref);
}

const struct super_operations ext4_sops = {
	.deinit_inode = ext4fs_deinit_inode,
	.read_inode = ext4fs_read_inode,
};
