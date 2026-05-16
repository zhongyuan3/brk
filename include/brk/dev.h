#ifndef BRK_DEV_H
#define BRK_DEV_H

#include <brk/bitmap.h> /* BITS_TO_LONGS */
#include <brk/list.h>
#include <brk/lock.h>
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

#define BRK_DEV_FIRST_DYNAMIC_MAJOR 16u
#define BRK_DEV_ALLOC_MINOR_SCAN 4096u

struct chrdev;
struct blkdev;
struct file;
struct address_space;

struct chrdev_operations {
	int (*open)(struct chrdev *cd, struct file *file);
	void (*release)(struct chrdev *cd, struct file *file);
	int (*read)(struct file *file, char *buf, usize_t n, usize_t *read);
	int (*write)(struct file *file, const char *buf, usize_t n,
		     usize_t *written);
	long (*ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
};

struct blkdev_operations {
	int (*read)(struct blkdev *bd, u64 blk_id, void *buf, u32 blk_cnt);
	int (*write)(struct blkdev *bd, u64 blk_id, const void *buf,
		     u32 blk_cnt);
};

struct chrdev {
	struct list_head list;
	dev_t dev;
	struct chrdev_operations ops;
};

struct blkdev {
	struct list_head list;
	dev_t dev;
	u32 phy_bsize;
	u64 phy_bcnt;
	struct blkdev_operations ops;
	void *priv;
	struct address_space *bd_mapping;
};

int blkdev_check_bounds(struct blkdev *bd, u64 blk_id, u32 blk_cnt);

int bdev_read_page(struct blkdev *bd, u64 index, void *buf);
int bdev_write_page(struct blkdev *bd, u64 index, const void *buf);

struct chrdev *chrdev_alloc(void);
void chrdev_free(struct chrdev *cd);

int chrdev_register(struct chrdev *cd, dev_t dev);
void chrdev_unregister(struct chrdev *cd);
struct chrdev *chrdev_get(dev_t dev);

int chrdev_alloc_major(unsigned *major_out);
void chrdev_free_major(unsigned major);
int chrdev_alloc_minor(unsigned major, unsigned *minor_out);
int chrdev_alloc_devnum(dev_t *dev_out);

struct blkdev *blkdev_alloc(void);
void blkdev_free(struct blkdev *bd);

int blkdev_register(struct blkdev *bd, dev_t dev);
void blkdev_unregister(struct blkdev *bd);
struct blkdev *blkdev_get(dev_t dev);

int blkdev_alloc_major(unsigned *major_out);
void blkdev_free_major(unsigned major);
int blkdev_alloc_minor(unsigned major, unsigned *minor_out);
int blkdev_alloc_devnum(dev_t *dev_out);

void dev_init(void);

int dev_console_init(void);
int virtio_disk_init(void);

struct dev_slot {
	struct list_head list;
	dev_t dev;
};

struct dev_registry {
	struct list_head lists[BRK_MAJOR_MAX];
	spinlock_t lock;
	unsigned long major_pooled[BITS_TO_LONGS(BRK_MAJOR_MAX)];
};

void dev_registry_init(struct dev_registry *reg);
int dev_registry_register(struct dev_registry *reg, struct dev_slot *slot,
			  dev_t dev, unsigned int dev_type);
void dev_registry_unregister(struct dev_registry *reg, struct dev_slot *slot);
struct dev_slot *dev_registry_get(struct dev_registry *reg, dev_t dev);
int dev_registry_alloc_major(struct dev_registry *reg, unsigned *major_out);
void dev_registry_free_major(struct dev_registry *reg, unsigned major);
int dev_registry_alloc_minor(struct dev_registry *reg, unsigned major,
			     unsigned *minor_out, unsigned int dev_type);
int dev_registry_alloc_devnum(struct dev_registry *reg, dev_t *dev_out,
			      unsigned int dev_type);

#endif
