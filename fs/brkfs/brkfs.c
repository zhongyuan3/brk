#include "brkfs.h"
#include <brk/bitmap.h>
#include <brk/dev.h>
#include <brk/dirent.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/string.h>
#include <brk/types.h>

struct brkfs_sb_info *brkfs_sb_info_alloc(struct blkdev *bdev,
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

	return sbi;
}

void brkfs_sb_info_free(struct brkfs_sb_info *sbi)
{
	kfree(sbi);
}

int brkfs_block_read(struct brkfs_sb_info *sb, uint32_t bno, void *buf)
{
	if (bno >= sb->s_sb.s_blocks_count) {
		klog_warn("%s(): Invalid bno: %u, blocks count: %u\n", __func__,
			 bno, sb->s_sb.s_blocks_count);
		return -EINVAL;
	}
	struct blkdev *bdev = sb->s_bdev;
	uint32_t bdev_bno = bno * sb->s_sb.s_blocksize / bdev->phy_bsize;
	uint32_t bdev_bcnt = sb->s_sb.s_blocksize / bdev->phy_bsize;
	return bdev->ops->read(bdev, bdev_bno, buf, bdev_bcnt);
}

int brkfs_block_write(struct brkfs_sb_info *sb, uint32_t bno, const void *buf)
{
	if (bno >= sb->s_sb.s_blocks_count) {
		klog_warn("%s(): Invalid bno: %u, blocks count: %u\n", __func__,
			 bno, sb->s_sb.s_blocks_count);
		return -EINVAL;
	}
	struct blkdev *bdev = sb->s_bdev;
	uint32_t bdev_bno = bno * sb->s_sb.s_blocksize / bdev->phy_bsize;
	uint32_t bdev_bcnt = sb->s_sb.s_blocksize / bdev->phy_bsize;
	return bdev->ops->write(bdev, bdev_bno, buf, bdev_bcnt);
}

static int brkfs_bitmap_alloc(struct brkfs_sb_info *sbi, uint32_t start_bno,
			      uint32_t nbits, uint32_t *out_bit)
{
	uint32_t bits_per_blk = sbi->s_bits_per_block;
	uint32_t bmap_blks = div_ceil(nbits, bits_per_blk);
	uint32_t bno = start_bno;
	uint32_t end_bno = start_bno + bmap_blks;
	size_t bs = sbi->s_sb.s_blocksize;
	void *blk;
	int err;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (; bno < end_bno; ++bno) {
		err = brkfs_block_read(sbi, bno, blk);
		if (err)
			goto out;
		uint32_t n = nbits > bits_per_blk ? bits_per_blk : nbits;
		size_t bit = 0;
		if (bitmap_alloc_bit(blk, n, &bit)) {
			*out_bit = (bno - start_bno) * bits_per_blk + bit;
			err = brkfs_block_write(sbi, bno, blk);
			goto out;
		}
		nbits -= n;
	}
	err = -ENOSPC;
out:
	kfree(blk);
	return err;
}

static int brkfs_bitmap_free(struct brkfs_sb_info *sbi, uint32_t start_bno,
			     uint32_t nbits, uint32_t bit)
{
	if (bit >= nbits) {
		klog_warn("%s(): Invalid bit: %u, nbits: %u\n", __func__, bit,
			 nbits);
		return -EINVAL;
	}

	uint32_t bits_per_blk = sbi->s_bits_per_block;
	uint32_t bno = start_bno + bit / bits_per_blk;
	uint32_t lb = bit / bits_per_blk * bits_per_blk;
	uint32_t rem = nbits - lb;
	uint32_t n = rem > bits_per_blk ? bits_per_blk : rem;
	size_t bs = sbi->s_sb.s_blocksize;
	void *blk;
	int err;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;
	err = brkfs_block_read(sbi, bno, blk);
	if (err)
		goto out;
	bitmap_free_bit(blk, n, bit - lb);
	err = brkfs_block_write(sbi, bno, blk);
out:
	kfree(blk);
	return err;
}

int brkfs_data_alloc(struct brkfs_sb_info *sbi, uint32_t *bno)
{
	uint32_t bit = 0;
	int err = brkfs_bitmap_alloc(sbi, sbi->s_sb.s_data_block_bitmap,
				     sbi->s_sb.s_data_blocks_count, &bit);
	if (err)
		return err;
	*bno = sbi->s_sb.s_first_data_block + bit;
	return 0;
}

int brkfs_data_free(struct brkfs_sb_info *sbi, uint32_t bno)
{
	if (bno < sbi->s_sb.s_first_data_block) {
		klog_warn("%s(): Invalid bno: %u, first data block: %u\n",
			 __func__, bno, sbi->s_sb.s_first_data_block);
		return -EINVAL;
	}
	uint32_t bit = bno - sbi->s_sb.s_first_data_block;
	return brkfs_bitmap_free(sbi, sbi->s_sb.s_data_block_bitmap,
				 sbi->s_sb.s_data_blocks_count, bit);
}

int brkfs_inode_read(struct brkfs_sb_info *sbi, struct inode *inode)
{
	struct brkfs_inode *disk_i;
	struct brkfs_inode_info *info;
	uint32_t bno;
	size_t bs = sbi->s_sb.s_blocksize;
	uint8_t *blk;
	int err;
	uint32_t ino = inode->i_ino;
	uint32_t isize = sbi->s_sb.s_inode_size;

	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		klog_warn("%s(): Invalid ino: %u\n", __func__, ino);
		return -EINVAL;
	}

	bno = sbi->s_sb.s_inode_table + (ino - 1) / sbi->s_inodes_per_block;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;
	err = brkfs_block_read(sbi, bno, blk);
	if (err)
		goto out;

	uint32_t idx = (ino - 1) % sbi->s_inodes_per_block;
	disk_i = (struct brkfs_inode *)(blk + idx * isize);

	info = inode->i_private;

	inode->i_mode = disk_i->i_mode;
	inode->i_rdev = disk_i->i_rdev;
	inode->i_nlink = disk_i->i_nlink;
	inode->i_size = disk_i->i_size;
	memcpy(info->i_block, disk_i->i_block, sizeof(disk_i->i_block));
	inode->i_atime.tv_sec = disk_i->i_atime;
	inode->i_atime.tv_nsec = disk_i->i_atime_nsec;
	inode->i_mtime.tv_sec = disk_i->i_mtime;
	inode->i_mtime.tv_nsec = disk_i->i_mtime_nsec;
	inode->i_ctime.tv_sec = disk_i->i_ctime;
	inode->i_ctime.tv_nsec = disk_i->i_ctime_nsec;
	inode->i_uid = disk_i->i_uid;
	inode->i_gid = disk_i->i_gid;

out:
	kfree(blk);
	return err;
}

int brkfs_inode_write(struct brkfs_sb_info *sbi, struct inode *inode)
{
	uint32_t bno;
	uint32_t ino = inode->i_ino;
	size_t bs = sbi->s_sb.s_blocksize;
	uint8_t *blk;
	int err;
	struct brkfs_inode *disk_i;
	struct brkfs_inode_info *info;
	uint32_t isize = sbi->s_sb.s_inode_size;

	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		klog_warn("%s(): Invalid ino: %u\n", __func__, ino);
		return -EINVAL;
	}

	bno = sbi->s_sb.s_inode_table + (ino - 1) / sbi->s_inodes_per_block;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;
	err = brkfs_block_read(sbi, bno, blk);
	if (err)
		goto out;

	uint32_t idx = (ino - 1) % sbi->s_inodes_per_block;
	disk_i = (struct brkfs_inode *)(blk + idx * isize);

	info = inode->i_private;

	disk_i->i_mode = inode->i_mode;
	disk_i->i_rdev = inode->i_rdev;
	disk_i->i_nlink = inode->i_nlink;
	disk_i->i_size = inode->i_size;
	memcpy(disk_i->i_block, info->i_block, sizeof(disk_i->i_block));
	disk_i->i_atime = inode->i_atime.tv_sec;
	disk_i->i_atime_nsec = inode->i_atime.tv_nsec;
	disk_i->i_mtime = inode->i_mtime.tv_sec;
	disk_i->i_mtime_nsec = inode->i_mtime.tv_nsec;
	disk_i->i_ctime = inode->i_ctime.tv_sec;
	disk_i->i_ctime_nsec = inode->i_ctime.tv_nsec;
	disk_i->i_uid = inode->i_uid;
	disk_i->i_gid = inode->i_gid;

	err = brkfs_block_write(sbi, bno, blk);
out:
	kfree(blk);
	return err;
}

int brkfs_inode_alloc(struct brkfs_sb_info *sbi, uint32_t *ino)
{
	uint32_t bit = 0;
	int err = brkfs_bitmap_alloc(sbi, sbi->s_sb.s_inode_bitmap,
				     sbi->s_sb.s_inodes_count, &bit);
	if (err)
		return err;
	*ino = bit + 1;
	return 0;
}

int brkfs_inode_free(struct brkfs_sb_info *sbi, uint32_t ino)
{
	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		klog_warn("%s(): Invalid ino: %u\n", __func__, ino);
		return -EINVAL;
	}
	ino -= 1;
	return brkfs_bitmap_free(sbi, sbi->s_sb.s_inode_bitmap,
				 sbi->s_sb.s_inodes_count, ino);
}

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, uint32_t ino, umode_t mode,
			  unsigned int nlink, dev_t rdev)
{
	struct inode stub = { 0 };
	struct brkfs_inode_info info;

	memset(&info, 0, sizeof(info));
	stub.i_ino = ino;
	stub.i_mode = mode;
	stub.i_nlink = nlink;
	stub.i_size = 0;
	stub.i_rdev = rdev;
	stub.i_private = &info;
	inode_times_set_all_now(&stub);
	return brkfs_inode_write(sbi, &stub);
}

int brkfs_inode_getblk(struct inode *inode, loff_t off, uint32_t *bno,
		       unsigned flags, struct brkfs_sb_info *sbi)
{
	loff_t size = inode->i_size;
	struct brkfs_inode_info *inf = inode->i_private;
	uint32_t *blk_ptrs = inf->i_block;
	bool out_of_space = off >= size;
	bool create = (flags & BRKFS_GETBLK_CREATE) != 0;
	int err = 0;
	uint32_t bs = sbi->s_sb.s_blocksize;
	uint32_t ptrs_per_blk = sbi->s_sb.s_blocksize / sizeof(uint32_t);

	if (off < 0)
		return -EINVAL;

	if (out_of_space && !create)
		return -ENOSPC;

	uint32_t bi = off / sbi->s_sb.s_blocksize;

	if (bi < BRKFS_DIRECT_BLOCKS) {
		if (blk_ptrs[bi] == 0) {
			if (!out_of_space) { /* hole */
				*bno = 0;
				return 0;
			}
			if (!create)
				return -ENOSPC;
			uint32_t new_bno = 0;
			err = brkfs_data_alloc(sbi, &new_bno);
			if (err)
				return err;
			blk_ptrs[bi] = new_bno;
			*bno = new_bno;
			return 0;
		}
		*bno = blk_ptrs[bi];
		return 0;
	}

	bi -= BRKFS_DIRECT_BLOCKS;
	if (bi < ptrs_per_blk) {
		uint32_t idb = blk_ptrs[BRKFS_INDIRECT_BLOCK];
		uint32_t *idb_ptrs = kmalloc(bs);
		if (!idb_ptrs)
			return -ENOMEM;
		if (idb == 0) {
			if (!out_of_space) { /* hole */
				*bno = 0;
				return 0;
			}
			if (!create)
				return -ENOSPC;
			err = brkfs_data_alloc(sbi, &idb);
			if (err)
				return err;
			blk_ptrs[BRKFS_INDIRECT_BLOCK] = idb;
			memset(idb_ptrs, 0, bs);
		} else {
			err = brkfs_block_read(sbi, idb, idb_ptrs);
			if (err) {
				kfree(idb_ptrs);
				return err;
			}
		}
		if (idb_ptrs[bi] == 0) {
			if (!out_of_space) { /* hole */
				*bno = 0;
				kfree(idb_ptrs);
				return 0;
			}
			if (!create) {
				kfree(idb_ptrs);
				return -ENOSPC;
			}
			uint32_t new_bno = 0;
			err = brkfs_data_alloc(sbi, &new_bno);
			if (err) {
				kfree(idb_ptrs);
				return err;
			}
			idb_ptrs[bi] = new_bno;
			err = brkfs_block_write(sbi, idb, idb_ptrs);
			if (err) {
				brkfs_data_free(sbi, new_bno);
				kfree(idb_ptrs);
				return err;
			}
			*bno = new_bno;
			kfree(idb_ptrs);
			return 0;
		}
		*bno = idb_ptrs[bi];
		kfree(idb_ptrs);
		return 0;
	}

	klog_warn("%s(): Double indirect block not implemented\n", __func__);

	return -ENOSPC;
}

static uint8_t brkfs_mode_to_dt(umode_t mode)
{
	if (S_ISLNK(mode))
		return DT_LNK;
	if (S_ISREG(mode))
		return DT_REG;
	if (S_ISDIR(mode))
		return DT_DIR;
	if (S_ISCHR(mode))
		return DT_CHR;
	if (S_ISBLK(mode))
		return DT_BLK;
	if (S_ISFIFO(mode))
		return DT_FIFO;
	if (S_ISSOCK(mode))
		return DT_SOCK;
	return DT_UNKNOWN;
}

static size_t brkfs_dirent_reclen(unsigned int name_len)
{
	return round_up(offsetof(struct brkfs_dir_entry, name) + name_len, 4u);
}

static int brkfs_dir_ensure_first_block(struct inode *dir)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *info = dir->i_private;
	uint32_t bno;
	uint8_t *blk;
	size_t bs = sbi->s_sb.s_blocksize;
	int err;

	if (dir->i_size > 0)
		return 0;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;
	err = brkfs_data_alloc(sbi, &bno);
	if (err) {
		kfree(blk);
		return err;
	}
	memset(blk, 0, bs);

	struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)blk;
	e->inode = 0;
	e->name_len = 0;
	e->file_type = 0;
	e->entry_len = (uint16_t)bs;

	info->i_block[0] = bno;
	dir->i_size = (loff_t)bs;
	err = brkfs_block_write(sbi, bno, blk);
	kfree(blk);
	return err;
}

static int __brkfs_dir_add_in_block(uint8_t *blk, size_t bs, uint32_t ino,
				    const char *name, unsigned int name_len,
				    uint8_t ftype, int32_t *walk_off)
{
	uint16_t n = bs;
	uint16_t new_min = brkfs_dirent_reclen(name_len);
	struct brkfs_dir_entry *ent = (struct brkfs_dir_entry *)blk;
	struct brkfs_dir_entry *new_ent;

	while (n >= BRKFS_DIR_ENTRY_MIN_LEN) {
		uint16_t ent_len = ent->entry_len;
		uint16_t ent_min = brkfs_dirent_reclen(ent->name_len);

		if (ent->inode == 0 && ent_len >= new_min) {
			uint16_t rest = ent_len - new_min;

			new_ent = ent;
			new_ent->inode = ino;
			new_ent->name_len = name_len;
			new_ent->file_type = ftype;
			new_ent->entry_len = new_min;
			memcpy(new_ent->name, name, name_len);
			if (rest >= BRKFS_DIR_ENTRY_MIN_LEN) {
				struct brkfs_dir_entry *hole =
					(struct brkfs_dir_entry
						 *)((uint8_t *)new_ent +
						    new_min);

				hole->inode = 0;
				hole->name_len = 0;
				hole->file_type = 0;
				hole->entry_len = rest;
			}
			return 0;
		}
		if (ent->inode != 0 && ent_len - ent_min >= new_min) {
			ent->entry_len = ent_min;
			new_ent = (struct brkfs_dir_entry *)((uint8_t *)ent +
							     ent_min);
			new_ent->entry_len = ent_len - ent_min;
			new_ent->inode = ino;
			new_ent->name_len = name_len;
			new_ent->file_type = ftype;
			memcpy(new_ent->name, name, name_len);
			return 0;
		}

		*walk_off += ent_len;
		ent = (struct brkfs_dir_entry *)((uint8_t *)ent + ent_len);
		n -= ent_len;
	}

	return -ENOSPC;
}

int brkfs_dir_add(struct inode *dir, uint32_t child_ino, const char *name,
		  unsigned int name_len, umode_t child_mode)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *di = dir->i_private;
	size_t bs = sbi->s_sb.s_blocksize;
	uint8_t ftype = brkfs_mode_to_dt(child_mode);
	uint8_t *blk;
	unsigned int bi;
	int err;
	int32_t walk;

	if (ftype == DT_UNKNOWN)
		return -EINVAL;

	err = brkfs_dir_ensure_first_block(dir);
	if (err)
		return err;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		uint32_t bno = di->i_block[bi];
		loff_t blk_base = bi * bs;

		if (blk_base >= dir->i_size)
			break;
		if (bno == 0)
			continue;

		err = brkfs_block_read(sbi, bno, blk);
		if (err)
			goto out;
		walk = blk_base;
		if (__brkfs_dir_add_in_block(blk, bs, child_ino, name, name_len,
					     ftype, &walk) == 0) {
			err = brkfs_block_write(sbi, bno, blk);
			goto out;
		}
	}

	if (bi < BRKFS_DIRECT_BLOCKS) {
		uint32_t bno;

		err = brkfs_data_alloc(sbi, &bno);
		if (err)
			goto out;
		memset(blk, 0, bs);

		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)blk;
		e->inode = 0;
		e->name_len = 0;
		e->file_type = 0;
		e->entry_len = bs;

		di->i_block[bi] = bno;
		dir->i_size = (bi + 1) * bs;
		walk = bi * bs;
		if (__brkfs_dir_add_in_block(blk, bs, child_ino, name, name_len,
					     ftype, &walk) == 0)
			err = brkfs_block_write(sbi, bno, blk);
		else
			err = -ENOMEM;
	} else {
		err = -ENOSPC;
	}

out:
	kfree(blk);
	return err;
}

static int __brkfs_dir_remove_in_block(uint8_t *blk, size_t bs,
				       unsigned int name_len, const char *name)
{
	struct brkfs_dir_entry *curr = (struct brkfs_dir_entry *)blk;
	struct brkfs_dir_entry *prev = NULL;
	uint16_t n = bs;
	unsigned int l;

	while (n >= BRKFS_DIR_ENTRY_MIN_LEN) {
		uint16_t ent_len = curr->entry_len;

		l = name_len < curr->name_len ? name_len : curr->name_len;
		if (curr->inode != 0 && curr->name_len == name_len &&
		    !memcmp(curr->name, name, l)) {
			if (prev)
				prev->entry_len += ent_len;
			else
				curr->inode = 0;
			return 0;
		}

		prev = curr;
		n -= ent_len;
		curr = (struct brkfs_dir_entry *)((uint8_t *)curr + ent_len);
	}

	return -ENOENT;
}

int brkfs_new_dir_body(struct inode *inode, uint32_t parent_ino)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *di = inode->i_private;
	size_t bs = sbi->s_sb.s_blocksize;
	uint32_t self_ino = inode->i_ino;
	uint32_t bno;
	uint8_t *blk;
	struct brkfs_dir_entry *e;
	struct brkfs_dir_entry *e2;
	struct brkfs_dir_entry *hole;
	size_t r1, r2, rest;
	int err;

	if (!di || !S_ISDIR(inode->i_mode))
		return -EINVAL;
	if (inode->i_size != 0)
		return 0;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;
	memset(blk, 0, bs);

	r1 = brkfs_dirent_reclen(1);
	r2 = brkfs_dirent_reclen(2);
	if (r1 + r2 + BRKFS_DIR_ENTRY_MIN_LEN > bs) {
		kfree(blk);
		return -EIO;
	}

	e = (struct brkfs_dir_entry *)blk;
	e->inode = self_ino;
	e->name_len = 1;
	e->file_type = DT_DIR;
	e->entry_len = r1;
	e->name[0] = '.';

	e2 = (struct brkfs_dir_entry *)(blk + r1);
	e2->inode = parent_ino;
	e2->name_len = 2;
	e2->file_type = DT_DIR;
	e2->entry_len = r2;
	e2->name[0] = '.';
	e2->name[1] = '.';

	rest = bs - r1 - r2;
	hole = (struct brkfs_dir_entry *)(blk + r1 + r2);
	hole->inode = 0;
	hole->name_len = 0;
	hole->file_type = 0;
	hole->entry_len = rest;

	err = brkfs_data_alloc(sbi, &bno);
	if (err) {
		kfree(blk);
		return err;
	}
	di->i_block[0] = bno;
	inode->i_size = bs;
	err = brkfs_block_write(sbi, bno, blk);
	kfree(blk);
	return err;
}

int brkfs_dir_remove(struct inode *dir, const char *name, unsigned int name_len)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *di = dir->i_private;
	size_t bs = sbi->s_sb.s_blocksize;
	uint8_t *blk;
	unsigned int bi;
	int err = -ENOENT;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		uint32_t bno = di->i_block[bi];
		loff_t blk_base = bi * bs;

		if (blk_base >= dir->i_size)
			break;
		if (bno == 0)
			continue;

		err = brkfs_block_read(sbi, bno, blk);
		if (err)
			goto out;
		if (__brkfs_dir_remove_in_block(blk, bs, name_len, name) == 0) {
			err = brkfs_block_write(sbi, bno, blk);
			goto out;
		}
	}

out:
	kfree(blk);
	return err;
}

int brkfs_dir_lookup(struct inode *dir, const char *name, unsigned int name_len,
		     uint32_t *ino_out, uint8_t *type_out)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *di = dir->i_private;
	size_t bs = sbi->s_sb.s_blocksize;
	uint8_t *blk;
	unsigned int bi;

	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		uint32_t bno = di->i_block[bi];
		loff_t blk_base = bi * bs;
		struct brkfs_dir_entry *ent;
		size_t left;

		if (blk_base >= dir->i_size)
			break;
		if (bno == 0)
			continue;

		int err = brkfs_block_read(sbi, bno, blk);
		if (err) {
			kfree(blk);
			return err;
		}
		ent = (struct brkfs_dir_entry *)blk;
		left = bs;
		while (left >= BRKFS_DIR_ENTRY_MIN_LEN) {
			uint16_t el = ent->entry_len;

			if (el < BRKFS_DIR_ENTRY_MIN_LEN || el > left) {
				kfree(blk);
				return -EIO;
			}
			if (ent->inode != 0 && ent->name_len == name_len &&
			    !memcmp(ent->name, name, name_len)) {
				*ino_out = ent->inode;
				if (type_out)
					*type_out = ent->file_type;
				kfree(blk);
				return 0;
			}
			ent = (struct brkfs_dir_entry *)((uint8_t *)ent + el);
			left -= el;
		}
	}

	return -ENOENT;
}

int brkfs_file_read_at(struct inode *inode, loff_t *pos, void *buf, size_t size,
		       size_t *read_out)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *inf = inode->i_private;
	uint32_t bs = sbi->s_sb.s_blocksize;
	loff_t off = *pos;
	int err = 0;
	uint8_t *blk = NULL;

	if (!inf || (!S_ISREG(inode->i_mode) && !S_ISLNK(inode->i_mode))) {
		*read_out = 0;
		return -EINVAL;
	}
	if (off >= inode->i_size) {
		*read_out = 0;
		return 0;
	}
	if ((loff_t)(off + size) > inode->i_size)
		size = (size_t)(inode->i_size - off);

	blk = kmalloc(bs);
	if (!blk) {
		*read_out = 0;
		return -ENOMEM;
	}

	uint8_t *p = buf;
	while (size > 0) {
		uint32_t bno;
		size_t in_off = (size_t)(off % (loff_t)bs);
		size_t chunk = bs - in_off;
		if (chunk > size)
			chunk = size;

		err = brkfs_inode_getblk(inode, off, &bno, 0, sbi);
		if (err)
			break;
		if (bno == 0) { /* hole */
			memset(p, 0, chunk);
		} else {
			err = brkfs_block_read(sbi, bno, blk);
			if (err)
				break;
			memcpy(p, blk + in_off, chunk);
		}

		off += chunk;
		size -= chunk;
		p += chunk;
	}

	*read_out = p - (uint8_t *)buf;
	*pos = off;
	kfree(blk);
	return err;
}

int brkfs_file_write_at(struct inode *inode, loff_t *pos, const void *buf,
			size_t size, size_t *written_out)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *inf = inode->i_private;
	uint32_t bs = sbi->s_sb.s_blocksize;
	loff_t off = *pos;
	int err = 0;
	uint8_t *blk = NULL;

	if (!inf || (!S_ISREG(inode->i_mode) && !S_ISLNK(inode->i_mode))) {
		*written_out = 0;
		return -EINVAL;
	}

	blk = kmalloc(bs);
	if (!blk) {
		*written_out = 0;
		return -ENOMEM;
	}

	const uint8_t *p = buf;
	while (size > 0) {
		uint32_t bno;
		size_t in_off = (size_t)(off % (loff_t)bs);
		size_t chunk = bs - in_off;
		if (chunk > size)
			chunk = size;

		err = brkfs_inode_getblk(inode, off, &bno, BRKFS_GETBLK_CREATE,
					 sbi);
		if (err)
			break;

		if (bno == 0) { /* should not happen */
			err = -ENOSPC;
			break;
		}

		err = brkfs_block_read(sbi, bno, blk);
		if (err)
			break;
		memcpy(blk + in_off, p, chunk);
		err = brkfs_block_write(sbi, bno, blk);
		if (err)
			break;
		memcpy(blk + in_off, p, chunk);
		err = brkfs_block_write(sbi, bno, blk);
		if (err)
			break;

		off += chunk;
		p += chunk;
		size -= chunk;
		if (off > inode->i_size)
			inode->i_size = off;
	}

	*written_out = p - (const uint8_t *)buf;
	*pos = off;
	if (!err && *written_out > 0)
		inode_touch_mtime(inode);
	kfree(blk);
	return err;
}

int brkfs_truncate_inode_blocks(struct inode *inode, loff_t new_size)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *inf = inode->i_private;
	uint32_t bs = sbi->s_sb.s_blocksize;
	loff_t old_size = inode->i_size;
	uint32_t old_n;
	uint32_t new_n;
	uint32_t bi;

	if (!inf)
		return -EINVAL;
	if (new_size < 0)
		return -EINVAL;
	if (new_size >= old_size) {
		inode->i_size = new_size;
		return 0;
	}

	old_n = (uint32_t)((old_size + (loff_t)bs - 1) / (loff_t)bs);
	new_n = (uint32_t)((new_size + (loff_t)bs - 1) / (loff_t)bs);

	for (bi = new_n; bi < old_n && bi < BRKFS_DIRECT_BLOCKS; bi++) {
		if (inf->i_block[bi]) {
			brkfs_data_free(sbi, inf->i_block[bi]);
			inf->i_block[bi] = 0;
		}
	}
	inode->i_size = new_size;
	return 0;
}
