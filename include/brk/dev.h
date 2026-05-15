#ifndef BRK_DEV_H
#define BRK_DEV_H

#include <brk/types.h>

/*
 * Device number encoding (dev_t):
 *   bits 28..31 : device class (CHRDEV / BLKDEV)
 *   bits 20..27 : major (8 bits, 0..255)
 *   bits  0..19 : minor (20 bits)
 */
#define BRK_MAJOR_BITS 8
#define BRK_MINOR_BITS 20
#define BRK_MAJOR_MAX (1u << BRK_MAJOR_BITS)
#define BRK_MINOR_MAX (1u << BRK_MINOR_BITS)

#define MKDEV(type, major, minor)                              \
	((((type) & 0xfu) << 28) | (((major) & 0xffu) << 20) | \
	 ((minor) & (BRK_MINOR_MAX - 1)))
#define MAJOR(dev) (((dev) >> 20) & 0xffu)
#define MINOR(dev) ((dev) & (BRK_MINOR_MAX - 1))
#define DEVTYPE(dev) (((dev) >> 28) & 0xfu)

#define CHRDEV 0
#define BLKDEV 1

#define IS_CHRDEV(dev) (DEVTYPE(dev) == CHRDEV)
#define IS_BLKDEV(dev) (DEVTYPE(dev) == BLKDEV)

/* Well-known nodes (majors below BRK_DEV_FIRST_DYNAMIC_MAJOR are policy-reserved) */
#define DEV_CONSOLE0 MKDEV(CHRDEV, 1, 0)
#define DEV_DISK0 MKDEV(BLKDEV, 1, 0)

#define DISK0_SIZE (4096 * 1024)

/*
 * Majors in [0, BRK_DEV_FIRST_DYNAMIC_MAJOR) are for static / board-defined
 * devices. chrdev_alloc_major / blkdev_alloc_major only assign from the
 * dynamic range.
 */
#define BRK_DEV_FIRST_DYNAMIC_MAJOR 16u
/*
 * When scanning for a free minor, stop at this bound to keep allocation O(1)
 * in practice for this kernel.
 */
#define BRK_DEV_ALLOC_MINOR_SCAN 4096u

struct chrdev;
struct chrdev_operations;
struct blkdev;
struct blkdev_operations;
struct address_space;

struct chrdev {
	dev_t dev;
	struct chrdev_operations *ops;
	struct list_head list;
};

struct file;

struct chrdev_operations {
	int (*read)(struct file *file, char *buf, usize_t n, usize_t *read);
	int (*write)(struct file *file, const char *buf, usize_t n,
		     usize_t *written);
	long (*ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
};

struct blkdev {
	struct list_head list;
	dev_t dev;
	u32 phy_bsize;
	u64 phy_bcnt;
	struct blkdev_operations *ops;
	void *priv;
	/*
	 * Page cache that backs every read/write made through
	 * bdev_read_page / bdev_write_page. Sized in PAGE_SIZE units;
	 * each cached_page maps a contiguous PAGE_SIZE region of the
	 * device starting at index << PAGE_SHIFT bytes.
	 */
	struct address_space *bd_mapping;
};

struct blkdev_operations {
	int (*read)(struct blkdev *bd, u64 blk_id, void *buf, u32 blk_cnt);
	int (*write)(struct blkdev *bd, u64 blk_id, const void *buf,
		     u32 blk_cnt);
};

/*
 * Page-cached helpers built on top of blkdev_operations. Index is in
 * PAGE_SIZE units of the device. @buf must be PAGE_SIZE bytes. Writes
 * are write-through: the page in the bdev mapping is updated and the
 * data is synchronously pushed to the underlying device before
 * returning, so metadata consumers (super, bitmap, inode table) get
 * strict ordering / durability semantics for free.
 */
int bdev_read_page(struct blkdev *bd, u64 index, void *buf);
int bdev_write_page(struct blkdev *bd, u64 index, const void *buf);

struct chrdev *chrdev_alloc(void);
void chrdev_free(struct chrdev *cd);

/* Register at an explicit (type, major, minor); dev must match CHRDEV. */
int chrdev_register(struct chrdev *cd, dev_t dev);
void chrdev_unregister(struct chrdev *cd);
struct chrdev *chrdev_get(dev_t dev);

/*
 * Major allocation (dynamic range only). A major stays claimed until
 * chrdev_free_major() after the last character device on that major unregisters,
 * or chrdev_free_major() is called explicitly on an empty major.
 */
int chrdev_alloc_major(unsigned *major_out);
void chrdev_free_major(unsigned major);

/* First unused minor in [0, BRK_DEV_ALLOC_MINOR_SCAN) for this major. */
int chrdev_alloc_minor(unsigned major, unsigned *minor_out);

struct blkdev *blkdev_alloc(void);
void blkdev_free(struct blkdev *bd);

int blkdev_register(struct blkdev *bd, dev_t dev);
void blkdev_unregister(struct blkdev *bd);
struct blkdev *blkdev_get(dev_t dev);

int blkdev_alloc_major(unsigned *major_out);
void blkdev_free_major(unsigned major);
int blkdev_alloc_minor(unsigned major, unsigned *minor_out);

/*
 * Allocate a full dev_t: prefers minor 0, then scans minors; majors start at
 * BRK_DEV_FIRST_DYNAMIC_MAJOR. Fails with -ENOMEM if no slot.
 */
int chrdev_alloc_devnum(dev_t *dev_out);
int blkdev_alloc_devnum(dev_t *dev_out);

void dev_init(void);

#endif
