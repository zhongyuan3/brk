#include "brkfs.h"
#include <brk/asm.h>
#include <brk/bitmap.h>
#include <brk/blkdev.h>
#include <brk/device.h>
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

/*
 * brkfs metadata I/O goes through the bdev page cache.
 *
 * brkfs is set up with s_blocksize == PAGE_SIZE, so every brkfs block
 * maps to exactly one page in the underlying bdev mapping. Reads can
 * thus return cached data with no disk traffic on a hit; writes are
 * write-through (see bdev_write_page) so existing brkfs callers retain
 * their "write returns => data is durable" expectation.
 */
int brkfs_block_read(struct brkfs_sb_info *sb, u32 bno, void *buf)
{
	if (bno >= sb->s_sb.s_blocks_count) {
		klog_warn("%s(): Invalid bno: %u, blocks count: %u\n", __func__,
			  bno, sb->s_sb.s_blocks_count);
		return -EINVAL;
	}
	if (sb->s_sb.s_blocksize != PAGE_SIZE)
		return -EIO;
	return bdev_read_page(sb->s_bdev, bno, buf);
}

int brkfs_block_write(struct brkfs_sb_info *sb, u32 bno, const void *buf)
{
	if (bno >= sb->s_sb.s_blocks_count) {
		klog_warn("%s(): Invalid bno: %u, blocks count: %u\n", __func__,
			  bno, sb->s_sb.s_blocks_count);
		return -EINVAL;
	}
	if (sb->s_sb.s_blocksize != PAGE_SIZE)
		return -EIO;
	return bdev_write_page(sb->s_bdev, bno, buf);
}

static int brkfs_bitmap_alloc(struct brkfs_sb_info *sbi, u32 start_bno,
			      u32 nbits, u32 *out_bit)
{
	u32 bits_per_blk = sbi->s_bits_per_block;
	u32 bmap_blks = div_ceil(nbits, bits_per_blk);
	u32 bno = start_bno;
	u32 end_bno = start_bno + bmap_blks;
	usize_t bs = sbi->s_sb.s_blocksize;
	void *blk;
	int err;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (; bno < end_bno; ++bno) {
		err = brkfs_block_read(sbi, bno, blk);
		if (err)
			goto out;
		u32 n = nbits > bits_per_blk ? bits_per_blk : nbits;
		usize_t bit = 0;
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

static int brkfs_bitmap_free(struct brkfs_sb_info *sbi, u32 start_bno,
			     u32 nbits, u32 bit)
{
	if (bit >= nbits) {
		klog_warn("%s(): Invalid bit: %u, nbits: %u\n", __func__, bit,
			  nbits);
		return -EINVAL;
	}

	u32 bits_per_blk = sbi->s_bits_per_block;
	u32 bno = start_bno + bit / bits_per_blk;
	u32 lb = bit / bits_per_blk * bits_per_blk;
	u32 rem = nbits - lb;
	u32 n = rem > bits_per_blk ? bits_per_blk : rem;
	usize_t bs = sbi->s_sb.s_blocksize;
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

int brkfs_data_alloc(struct brkfs_sb_info *sbi, u32 *bno)
{
	u32 bit = 0;
	int err = brkfs_bitmap_alloc(sbi, sbi->s_sb.s_data_block_bitmap,
				     sbi->s_sb.s_data_blocks_count, &bit);
	if (err)
		return err;
	*bno = sbi->s_sb.s_first_data_block + bit;
	return 0;
}

int brkfs_data_free(struct brkfs_sb_info *sbi, u32 bno)
{
	if (bno < sbi->s_sb.s_first_data_block) {
		klog_warn("%s(): Invalid bno: %u, first data block: %u\n",
			  __func__, bno, sbi->s_sb.s_first_data_block);
		return -EINVAL;
	}
	u32 bit = bno - sbi->s_sb.s_first_data_block;
	return brkfs_bitmap_free(sbi, sbi->s_sb.s_data_block_bitmap,
				 sbi->s_sb.s_data_blocks_count, bit);
}

int brkfs_inode_read(struct brkfs_sb_info *sbi, struct fs_inode *inode)
{
	struct brkfs_inode *disk_i;
	struct brkfs_inode_info *info;
	u32 bno;
	usize_t bs = sbi->s_sb.s_blocksize;
	u8 *blk;
	int err;
	u32 ino = inode->i_ino;
	u32 isize = sbi->s_sb.s_inode_size;

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

	u32 idx = (ino - 1) % sbi->s_inodes_per_block;
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

int brkfs_inode_write(struct brkfs_sb_info *sbi, struct fs_inode *inode)
{
	u32 bno;
	u32 ino = inode->i_ino;
	usize_t bs = sbi->s_sb.s_blocksize;
	u8 *blk;
	int err;
	struct brkfs_inode *disk_i;
	struct brkfs_inode_info *info;
	u32 isize = sbi->s_sb.s_inode_size;

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

	u32 idx = (ino - 1) % sbi->s_inodes_per_block;
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

int brkfs_inode_alloc(struct brkfs_sb_info *sbi, u32 *ino)
{
	u32 bit = 0;
	int err = brkfs_bitmap_alloc(sbi, sbi->s_sb.s_inode_bitmap,
				     sbi->s_sb.s_inodes_count, &bit);
	if (err)
		return err;
	*ino = bit + 1;
	return 0;
}

int brkfs_inode_free(struct brkfs_sb_info *sbi, u32 ino)
{
	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		klog_warn("%s(): Invalid ino: %u\n", __func__, ino);
		return -EINVAL;
	}
	ino -= 1;
	return brkfs_bitmap_free(sbi, sbi->s_sb.s_inode_bitmap,
				 sbi->s_sb.s_inodes_count, ino);
}

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, u32 ino, umode_t mode,
			  unsigned int nlink, dev_t rdev)
{
	struct fs_inode stub = { 0 };
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

int brkfs_inode_getblk(struct fs_inode *inode, loff_t off, u32 *bno,
		       unsigned flags, struct brkfs_sb_info *sbi)
{
	struct brkfs_inode_info *inf = inode->i_private;
	u32 *blk_ptrs = inf->i_block;
	bool create = (flags & BRKFS_GETBLK_CREATE) != 0;
	int err = 0;
	u32 bs = sbi->s_sb.s_blocksize;
	u32 ptrs_per_blk = sbi->s_sb.s_blocksize / sizeof(u32);

	if (off < 0)
		return -EINVAL;

	u32 bi = off / sbi->s_sb.s_blocksize;

	if (bi < BRKFS_DIRECT_BLOCKS) {
		if (blk_ptrs[bi] == 0) {
			if (!create) { /* read of a hole or sparse region */
				*bno = 0;
				return 0;
			}
			u32 new_bno = 0;
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
		u32 idb = blk_ptrs[BRKFS_INDIRECT_BLOCK];
		u32 *idb_ptrs = kmalloc(bs);
		if (!idb_ptrs)
			return -ENOMEM;
		if (idb == 0) {
			if (!create) {
				*bno = 0;
				kfree(idb_ptrs);
				return 0;
			}
			err = brkfs_data_alloc(sbi, &idb);
			if (err) {
				kfree(idb_ptrs);
				return err;
			}
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
			if (!create) {
				*bno = 0;
				kfree(idb_ptrs);
				return 0;
			}
			u32 new_bno = 0;
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

static u8 brkfs_mode_to_dt(umode_t mode)
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

static usize_t brkfs_dirent_reclen(unsigned int name_len)
{
	return round_up(offsetof(struct brkfs_dir_entry, name) + name_len, 4u);
}

static int brkfs_dir_ensure_first_block(struct fs_inode *dir)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *info = dir->i_private;
	u32 bno;
	u8 *blk;
	usize_t bs = sbi->s_sb.s_blocksize;
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
	e->entry_len = (u16)bs;

	info->i_block[0] = bno;
	dir->i_size = (loff_t)bs;
	err = brkfs_block_write(sbi, bno, blk);
	kfree(blk);
	return err;
}

static int __brkfs_dir_add_in_block(u8 *blk, usize_t bs, u32 ino,
				    const char *name, unsigned int name_len,
				    u8 ftype, s32 *walk_off)
{
	u16 n = bs;
	u16 new_min = brkfs_dirent_reclen(name_len);
	struct brkfs_dir_entry *ent = (struct brkfs_dir_entry *)blk;
	struct brkfs_dir_entry *new_ent;

	while (n >= BRKFS_DIR_ENTRY_MIN_LEN) {
		u16 ent_len = ent->entry_len;
		u16 ent_min = brkfs_dirent_reclen(ent->name_len);

		if (ent->inode == 0 && ent_len >= new_min) {
			u16 rest = ent_len - new_min;

			new_ent = ent;
			new_ent->inode = ino;
			new_ent->name_len = name_len;
			new_ent->file_type = ftype;
			new_ent->entry_len = new_min;
			memcpy(new_ent->name, name, name_len);
			if (rest >= BRKFS_DIR_ENTRY_MIN_LEN) {
				struct brkfs_dir_entry *hole =
					(struct brkfs_dir_entry
						 *)((u8 *)new_ent + new_min);

				hole->inode = 0;
				hole->name_len = 0;
				hole->file_type = 0;
				hole->entry_len = rest;
			}
			return 0;
		}
		if (ent->inode != 0 && ent_len - ent_min >= new_min) {
			ent->entry_len = ent_min;
			new_ent =
				(struct brkfs_dir_entry *)((u8 *)ent + ent_min);
			new_ent->entry_len = ent_len - ent_min;
			new_ent->inode = ino;
			new_ent->name_len = name_len;
			new_ent->file_type = ftype;
			memcpy(new_ent->name, name, name_len);
			return 0;
		}

		*walk_off += ent_len;
		ent = (struct brkfs_dir_entry *)((u8 *)ent + ent_len);
		n -= ent_len;
	}

	return -ENOSPC;
}

int brkfs_dir_add(struct fs_inode *dir, u32 child_ino, const char *name,
		  unsigned int name_len, umode_t child_mode)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *di = dir->i_private;
	usize_t bs = sbi->s_sb.s_blocksize;
	u8 ftype = brkfs_mode_to_dt(child_mode);
	u8 *blk;
	unsigned int bi;
	int err;
	s32 walk;

	if (ftype == DT_UNKNOWN)
		return -EINVAL;

	err = brkfs_dir_ensure_first_block(dir);
	if (err)
		return err;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 bno = di->i_block[bi];
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
		u32 bno;

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

static int __brkfs_dir_remove_in_block(u8 *blk, usize_t bs,
				       unsigned int name_len, const char *name)
{
	struct brkfs_dir_entry *curr = (struct brkfs_dir_entry *)blk;
	struct brkfs_dir_entry *prev = NULL;
	u16 n = bs;
	unsigned int l;

	while (n >= BRKFS_DIR_ENTRY_MIN_LEN) {
		u16 ent_len = curr->entry_len;

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
		curr = (struct brkfs_dir_entry *)((u8 *)curr + ent_len);
	}

	return -ENOENT;
}

int brkfs_new_dir_body(struct fs_inode *inode, u32 parent_ino)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *di = inode->i_private;
	usize_t bs = sbi->s_sb.s_blocksize;
	u32 self_ino = inode->i_ino;
	u32 bno;
	u8 *blk;
	struct brkfs_dir_entry *e;
	struct brkfs_dir_entry *e2;
	struct brkfs_dir_entry *hole;
	usize_t r1, r2, rest;
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

int brkfs_dir_remove(struct fs_inode *dir, const char *name,
		     unsigned int name_len)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *di = dir->i_private;
	usize_t bs = sbi->s_sb.s_blocksize;
	u8 *blk;
	unsigned int bi;
	int err = -ENOENT;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 bno = di->i_block[bi];
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

int brkfs_dir_lookup(struct fs_inode *dir, const char *name,
		     unsigned int name_len, u32 *ino_out, u8 *type_out)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct brkfs_inode_info *di = dir->i_private;
	usize_t bs = sbi->s_sb.s_blocksize;
	u8 *blk;
	unsigned int bi;

	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 bno = di->i_block[bi];
		loff_t blk_base = bi * bs;
		struct brkfs_dir_entry *ent;
		usize_t left;

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
			u16 el = ent->entry_len;

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
			ent = (struct brkfs_dir_entry *)((u8 *)ent + el);
			left -= el;
		}
	}

	return -ENOENT;
}

int brkfs_file_read_at(struct fs_inode *inode, loff_t *pos, void *buf,
		       usize_t size, usize_t *read_out)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *inf = inode->i_private;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t off = *pos;
	int err = 0;
	u8 *blk = NULL;

	if (!inf || (!S_ISREG(inode->i_mode) && !S_ISLNK(inode->i_mode))) {
		*read_out = 0;
		return -EINVAL;
	}
	if (off >= inode->i_size) {
		*read_out = 0;
		return 0;
	}
	if ((loff_t)(off + size) > inode->i_size)
		size = (usize_t)(inode->i_size - off);

	blk = kmalloc(bs);
	if (!blk) {
		*read_out = 0;
		return -ENOMEM;
	}

	u8 *p = buf;
	while (size > 0) {
		u32 bno;
		usize_t in_off = (usize_t)(off % (loff_t)bs);
		usize_t chunk = bs - in_off;
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

	*read_out = p - (u8 *)buf;
	*pos = off;
	kfree(blk);
	return err;
}

int brkfs_file_write_at(struct fs_inode *inode, loff_t *pos, const void *buf,
			usize_t size, usize_t *written_out)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *inf = inode->i_private;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t off = *pos;
	int err = 0;
	u8 *blk = NULL;

	if (!inf || (!S_ISREG(inode->i_mode) && !S_ISLNK(inode->i_mode))) {
		*written_out = 0;
		return -EINVAL;
	}

	blk = kmalloc(bs);
	if (!blk) {
		*written_out = 0;
		return -ENOMEM;
	}

	const u8 *p = buf;
	while (size > 0) {
		u32 bno;
		usize_t in_off = (usize_t)(off % (loff_t)bs);
		usize_t chunk = bs - in_off;
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

	*written_out = p - (const u8 *)buf;
	*pos = off;
	if (!err && *written_out > 0)
		inode_touch_mtime(inode);
	kfree(blk);
	return err;
}

int brkfs_truncate_inode_blocks(struct fs_inode *inode, loff_t new_size)
{
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *inf = inode->i_private;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t old_size = inode->i_size;
	u32 old_n;
	u32 new_n;
	u32 bi;

	if (!inf)
		return -EINVAL;
	if (new_size < 0)
		return -EINVAL;
	if (new_size >= old_size) {
		inode->i_size = new_size;
		return 0;
	}

	old_n = (u32)((old_size + (loff_t)bs - 1) / (loff_t)bs);
	new_n = (u32)((new_size + (loff_t)bs - 1) / (loff_t)bs);

	for (bi = new_n; bi < old_n && bi < BRKFS_DIRECT_BLOCKS; bi++) {
		if (inf->i_block[bi]) {
			brkfs_data_free(sbi, inf->i_block[bi]);
			inf->i_block[bi] = 0;
		}
	}
	inode->i_size = new_size;
	return 0;
}
