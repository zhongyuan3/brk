#ifndef BRK_BLKDEV_H
#define BRK_BLKDEV_H

#include <brk/device.h>
#include <brk/refcnt_types.h>
#include <brk/types.h>

struct block_dev;
struct page_cache;
struct cached_page;
struct request_queue;

struct block_dev {
	dev_t dev;
	uint32_t phy_bsize;
	uint64_t phy_bcnt;
	struct page_cache *bd_mapping;
	struct hlist_node hlist;
	refcnt_t ref;
	bool dead;

	/*
	 * Partition view.  A partition owns a reference to the whole-disk
	 * block_dev and forwards every I/O request at bd_start_sect blocks
	 * into it.  Both use the same phy_bsize.
	 */
	struct block_dev *bd_parent;
	uint64_t bd_start_sect;

	/* Request queue, only set on the whole-disk view. */
	struct request_queue *bd_queue;
};

void blkdev_registry_init(void);

int blkdev_check_bounds(struct block_dev *bd, uint64_t blk_id,
			uint32_t blk_cnt);
int blkdev_read(struct block_dev *bd, uint64_t blk_id, void *buf,
		uint32_t blk_cnt);
int blkdev_write(struct block_dev *bd, uint64_t blk_id, const void *buf,
		 uint32_t blk_cnt);
int blkdev_flush(struct block_dev *bd);
static inline uint64_t blkdev_capacity_bytes(const struct block_dev *bd)
{
	return bd->phy_bcnt * bd->phy_bsize;
}

struct block_dev *blkdev_alloc(void);
void blkdev_free(struct block_dev *bd);

int blkdev_register(struct block_dev *bd);
void blkdev_unregister(struct block_dev *bd);
struct block_dev *blkdev_get(dev_t dev);
void blkdev_put(struct block_dev *bd);

int blkdev_alloc_major(unsigned *major_out);
void blkdev_free_major(unsigned major);
int blkdev_alloc_minor(unsigned major, unsigned *minor_out);
void blkdev_free_minor(unsigned major, unsigned minor);
int blkdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out);
void blkdev_free_region(unsigned major, unsigned minor, unsigned count);

#endif
