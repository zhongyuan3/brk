#include <arch/asm/page.h>
#include <brk/bio.h>
#include <brk/blkdev.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/string.h>
#include <inttypes.h>
#include <uapi/errno.h>

int blkdev_check_bounds(struct block_dev *bd, u64 blk_id, u32 blk_cnt)
{
	if (blk_cnt == 0)
		return 0;
	if (blk_id >= bd->phy_bcnt) {
		pr_warn("%s(): invalid blk_id %" PRIu64 ", phy_bcnt %" PRIu64
			"\n",
			__func__, blk_id, bd->phy_bcnt);
		return -ENXIO;
	}
	if (bd->phy_bcnt - blk_id < blk_cnt) {
		pr_warn("%s(): invalid blk_cnt %" PRIu32 ", phy_bcnt %" PRIu64
			"\n",
			__func__, blk_cnt, bd->phy_bcnt);
		return -ENXIO;
	}
	return 0;
}

static int blkdev_submit_buf(struct block_dev *bd, u64 blk_id, void *buf,
			     size_t len, enum req_op op)
{
	size_t done = 0;
	int err;

	if (len == 0)
		return 0;
	if (bd->phy_bsize != 512 || (len & 511))
		return -EINVAL;

	err = blkdev_check_bounds(bd, blk_id, (u32)(len >> 9));
	if (err)
		return err;

	while (done < len) {
		struct bio bio;
		size_t chunk;
		unsigned int i;

		bio_init(&bio, bd, op);
		bio.bi_sector = blk_id + (done >> 9);

		for (i = 0; i < BIO_MAX_VECS && done < len; ++i) {
			chunk = len - done;
			if (chunk > PAGE_SIZE)
				chunk = PAGE_SIZE;

			err = bio_add_buf(&bio, (u8 *)buf + done, chunk);
			if (err)
				return err;
			done += chunk;
		}

		err = submit_bio_wait(&bio);
		if (err)
			return err;
	}

	return 0;
}

int blkdev_read(struct block_dev *bd, u64 blk_id, void *buf, u32 blk_cnt)
{
	if (!bd)
		return -EINVAL;
	return blkdev_submit_buf(bd, blk_id, buf,
				 (size_t)blk_cnt * bd->phy_bsize, REQ_OP_READ);
}

int blkdev_write(struct block_dev *bd, u64 blk_id, const void *buf, u32 blk_cnt)
{
	if (!bd)
		return -EINVAL;
	return blkdev_submit_buf(bd, blk_id, (void *)buf,
				 (size_t)blk_cnt * bd->phy_bsize, REQ_OP_WRITE);
}

int blkdev_flush(struct block_dev *bd)
{
	struct bio bio;

	if (!bd)
		return -EINVAL;

	bio_init(&bio, bd, REQ_OP_FLUSH);
	return submit_bio_wait(&bio);
}

static int blkdev_read_page(struct page_cache *m, struct cached_page *cp)
{
	struct block_dev *bd;
	u32 bs;
	u32 nsec;
	u32 valid;
	u64 sector;
	int err;
	void *buf;

	bd = m->host;
	if (!bd)
		return -EIO;
	bs = bd->phy_bsize;
	if (bs == 0 || bs > PAGE_SIZE || PAGE_SIZE % bs != 0)
		return -EIO;
	if (cp->page == NULL)
		return -EIO;

	nsec = PAGE_SIZE / bs;
	sector = cp->index * nsec;
	buf = (void *)page_to_virt(cp->page);

	if (sector >= bd->phy_bcnt) {
		memset(buf, 0, PAGE_SIZE);
		return 0;
	}

	valid = nsec;
	if (bd->phy_bcnt - sector < valid)
		valid = (u32)(bd->phy_bcnt - sector);

	err = blkdev_check_bounds(bd, sector, valid);
	if (err)
		return err;

	err = blkdev_read(bd, sector, buf, valid);
	if (err)
		return err;

	if (valid < nsec)
		memset((u8 *)buf + valid * bs, 0, PAGE_SIZE - valid * bs);

	return 0;
}

static int blkdev_write_page(struct page_cache *m, struct cached_page *cp)
{
	struct block_dev *bd;
	u32 bs;
	u32 nsec;
	u32 valid;
	u64 sector;
	int err;
	void *buf;

	bd = m->host;
	if (!bd)
		return -EIO;
	bs = bd->phy_bsize;
	if (bs == 0 || bs > PAGE_SIZE || PAGE_SIZE % bs != 0)
		return -EIO;

	nsec = PAGE_SIZE / bs;
	sector = cp->index * nsec;

	if (sector >= bd->phy_bcnt)
		return 0;

	valid = nsec;
	if (bd->phy_bcnt - sector < valid)
		valid = (u32)(bd->phy_bcnt - sector);

	err = blkdev_check_bounds(bd, sector, valid);
	if (err)
		return err;

	buf = (void *)page_to_virt(cp->page);
	return blkdev_write(bd, sector, buf, valid);
}

const struct page_cache_ops bdev_pc_ops = {
	.read_page = blkdev_read_page,
	.write_page = blkdev_write_page,
};
