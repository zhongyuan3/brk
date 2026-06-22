#include "brkfs.h"
#include <brk/bitmap.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

int brkfs_get_block(struct brkfs_sb_info *sbi, u32 bno, struct brkfs_block *bb)
{
	pgoff_t index;
	struct cached_page *cp;
	u8 *data;
	u32 bsize = sbi->s_sb.s_blocksize;

	if (bno >= sbi->s_sb.s_blocks_count)
		return -EINVAL;

	index = bno * bsize / PAGE_SIZE;
	cp = read_mapping_page(sbi->s_bdev->bd_mapping, index);
	if (IS_ERR(cp))
		return PTR_ERR(cp);

	data = cached_page_addr(cp);
	bb->cp = cp;
	bb->data = data + (bno * bsize % PAGE_SIZE);
	return 0;
}

void brkfs_put_block(struct brkfs_block *bb)
{
	cached_page_put(bb->cp);
	bb->cp = NULL;
	bb->data = NULL;
}

int brkfs_data_alloc(struct brkfs_sb_info *sbi, u32 *bno)
{
	u32 bit = 0;
	int err;

	err = brkfs_bitmap_alloc(sbi, sbi->s_sb.s_data_block_bitmap,
				 sbi->s_sb.s_data_blocks_count, &bit);
	if (err)
		return err;
	*bno = sbi->s_sb.s_first_data_block + bit;
	return 0;
}

int brkfs_data_free(struct brkfs_sb_info *sbi, u32 bno)
{
	u32 bit = bno - sbi->s_sb.s_first_data_block;

	if (bno < sbi->s_sb.s_first_data_block) {
		klog_warn("%s(): Invalid bno: %u, first data block: %u\n",
			  __func__, bno, sbi->s_sb.s_first_data_block);
		return -EINVAL;
	}

	return brkfs_bitmap_free(sbi, sbi->s_sb.s_data_block_bitmap,
				 sbi->s_sb.s_data_blocks_count, bit);
}

int brkfs_bitmap_alloc(struct brkfs_sb_info *sbi, u32 start_bno, u32 nbits,
		       u32 *out_bit)
{
	u32 bits_per_blk = sbi->s_bits_per_block;
	u32 nblks = div_ceil(nbits, bits_per_blk);
	u32 bno = start_bno;
	u32 end_bno = start_bno + nblks;
	int err;
	struct brkfs_block curr_bb, next_bb;

	err = brkfs_get_block(sbi, bno, &curr_bb);
	if (err)
		return err;

	while (1) {
		u32 n = nbits > bits_per_blk ? bits_per_blk : nbits;
		usize_t bit = 0;
		cached_page_lock(curr_bb.cp);
		if (bitmap_alloc_bit(curr_bb.data, n, &bit)) {
			cached_page_unlock(curr_bb.cp);
			*out_bit = (bno - start_bno) * bits_per_blk + bit;
			cached_page_mark_dirty(curr_bb.cp);
			brkfs_put_block(&curr_bb);
			return 0;
		}
		cached_page_unlock(curr_bb.cp);
		brkfs_put_block(&curr_bb);
		nbits -= n;

		++bno;
		if (bno >= end_bno)
			break;

		err = brkfs_get_block(sbi, bno, &next_bb);
		if (err) {
			brkfs_put_block(&curr_bb);
			return err;
		}
		brkfs_put_block(&curr_bb);
		curr_bb = next_bb;
	}

	return -ENOSPC;
}

int brkfs_bitmap_free(struct brkfs_sb_info *sbi, u32 start_bno, u32 nbits,
		      u32 bit)
{
	if (bit >= nbits) {
		klog_warn("%s(): Invalid bit: %u, nbits: %u\n", __func__, bit,
			  nbits);
		return -EINVAL;
	}

	u32 bits_per_blk = sbi->s_bits_per_block;
	u32 bno = start_bno + bit / bits_per_blk;
	u32 lb = bit / bits_per_blk * bits_per_blk;
	u32 n = nbits - lb;
	if (n > bits_per_blk)
		n = bits_per_blk;
	int err;
	struct brkfs_block bb;

	err = brkfs_get_block(sbi, bno, &bb);
	if (err)
		return err;
	cached_page_lock(bb.cp);
	bitmap_free_bit(bb.data, n, bit - lb);
	cached_page_mark_dirty(bb.cp);
	cached_page_unlock(bb.cp);
	brkfs_put_block(&bb);
	return 0;
}

int brkfs_block_read(struct brkfs_sb_info *sb, u32 bno, void *buf)
{
	struct brkfs_block bb;
	int err;

	err = brkfs_get_block(sb, bno, &bb);
	if (err)
		return err;
	memcpy(buf, bb.data, sb->s_sb.s_blocksize);
	brkfs_put_block(&bb);
	return 0;
}

int brkfs_block_write(struct brkfs_sb_info *sb, u32 bno, const void *buf)
{
	struct brkfs_block bb;
	int err;

	err = brkfs_get_block(sb, bno, &bb);
	if (err)
		return err;
	memcpy(bb.data, buf, sb->s_sb.s_blocksize);
	cached_page_mark_dirty(bb.cp);
	brkfs_put_block(&bb);
	return 0;
}
