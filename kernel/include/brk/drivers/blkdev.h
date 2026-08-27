#ifndef BRK_BLKDEV_H
#define BRK_BLKDEV_H

#include <brk/base/types.h>
#include <brk/drivers/device.h>

struct block_dev;
struct page_cache;
struct cached_page;

struct block_dev_ops {
	int (*read)(struct block_dev *, uint64_t, void *, uint32_t);
	int (*write)(struct block_dev *, uint64_t, const void *, uint32_t);
};

struct block_dev {
	dev_t dev;
	uint32_t phy_bsize;
	uint64_t phy_bcnt;
	struct block_dev_ops ops;
	void *priv;
	struct page_cache *bd_mapping;
	struct hlist_node hlist;
};

void blkdev_registry_init(void);

int blkdev_check_bounds(struct block_dev *bd, uint64_t blk_id,
			uint32_t blk_cnt);
int blkdev_read(struct block_dev *bd, uint64_t blk_id, void *buf,
		uint32_t blk_cnt);
int blkdev_write(struct block_dev *bd, uint64_t blk_id, const void *buf,
		 uint32_t blk_cnt);

struct block_dev *blkdev_alloc(void);
void blkdev_free(struct block_dev *bd);

int blkdev_register(struct block_dev *bd);
void blkdev_unregister(struct block_dev *bd);
struct block_dev *blkdev_get(dev_t dev);

int blkdev_alloc_major(unsigned *major_out);
void blkdev_free_major(unsigned major);
int blkdev_alloc_minor(unsigned major, unsigned *minor_out);
void blkdev_free_minor(unsigned major, unsigned minor);
int blkdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out);
void blkdev_free_region(unsigned major, unsigned minor, unsigned count);

#endif
