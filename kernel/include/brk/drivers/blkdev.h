#ifndef BRK_BLKDEV_H
#define BRK_BLKDEV_H

#include <brk/drivers/device.h>
#include <brk/lib/types.h>

struct block_dev;
struct page_cache;
struct cached_page;

struct block_dev_ops {
	int (*read)(struct block_dev *, u64, void *, u32);
	int (*write)(struct block_dev *, u64, const void *, u32);
};

struct block_dev {
	dev_t dev;
	u32 phy_bsize;
	u64 phy_bcnt;
	struct block_dev_ops ops;
	void *priv;
	struct page_cache *bd_mapping;
	struct hlist_node hlist;
};

void blkdev_registry_init(void);

int blkdev_check_bounds(struct block_dev *bd, u64 blk_id, u32 blk_cnt);
int blkdev_read(struct block_dev *bd, u64 blk_id, void *buf, u32 blk_cnt);
int blkdev_write(struct block_dev *bd, u64 blk_id, const void *buf,
		 u32 blk_cnt);

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
