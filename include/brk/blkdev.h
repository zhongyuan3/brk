#ifndef BRK_BLKDEV_H
#define BRK_BLKDEV_H

#include <brk/device.h>
#include <brk/types.h>

struct blkdev;
struct address_space;

struct blkdev_operations {
	int (*read)(struct blkdev *bd, u64 blk_id, void *buf, u32 blk_cnt);
	int (*write)(struct blkdev *bd, u64 blk_id, const void *buf,
		     u32 blk_cnt);
};

struct blkdev {
	dev_t dev;
	u32 phy_bsize;
	u64 phy_bcnt;
	struct blkdev_operations ops;
	void *priv;
	struct address_space *bd_mapping;
	struct hlist_node hlist;
};

void blkdev_registry_init(void);

int blkdev_check_bounds(struct blkdev *bd, u64 blk_id, u32 blk_cnt);

int bdev_read_page(struct blkdev *bd, u64 index, void *buf);
int bdev_write_page(struct blkdev *bd, u64 index, const void *buf);

struct blkdev *blkdev_alloc(void);
void blkdev_free(struct blkdev *bd);

int blkdev_register(struct blkdev *bd);
void blkdev_unregister(struct blkdev *bd);
struct blkdev *blkdev_get(dev_t dev);

int blkdev_alloc_major(unsigned *major_out);
void blkdev_free_major(unsigned major);
int blkdev_alloc_minor(unsigned major, unsigned *minor_out);
void blkdev_free_minor(unsigned major, unsigned minor);
int blkdev_alloc_region(unsigned major, unsigned base_minor, unsigned count,
			dev_t *dev_out);
void blkdev_free_region(unsigned major, unsigned minor, unsigned count);

#endif
