#ifndef BRK_DEV_H
#define BRK_DEV_H

#include <brk/types.h>

#define NR_DEVICES 32

#define MKDEV(type, major, minor)                            \
	((((type) & 0xf) << 28) | (((major) & 0xff) << 20) | \
	 ((minor) & 0xfffff))
#define MAJOR(dev) (((dev) >> 20) & 0xff)
#define MINOR(dev) ((dev) & 0xfffff)

#define CHRDEV 0
#define BLKDEV 1

#define IS_CHRDEV(dev) ((((dev) >> 28) & 0xf) == CHRDEV)
#define IS_BLKDEV(dev) ((((dev) >> 28) & 0xf) == BLKDEV)

#define DEV_CONSOLE0 MKDEV(CHRDEV, 1, 0)
#define DEV_DISK0 MKDEV(BLKDEV, 1, 0)

#define DISK0_SIZE (4096 * 1024)

struct chrdev;
struct chrdev_operations;
struct blkdev;
struct blkdev_operations;

struct chrdev {
	dev_t dev;
	struct chrdev_operations *ops;
	struct list_head list;
};

struct file;

struct chrdev_operations {
	int (*read)(struct file *file, char *buf, size_t n, size_t *read);
	int (*write)(struct file *file, const char *buf, size_t n,
		     size_t *written);
	long (*ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
};

struct blkdev {
	struct list_head list;
	dev_t dev;
	uint32_t phy_bsize;
	uint64_t phy_bcnt;
	struct blkdev_operations *ops;
	void *priv;
};

struct blkdev_operations {
	int (*read)(struct blkdev *bd, uint64_t blk_id, void *buf,
		    uint32_t blk_cnt);
	int (*write)(struct blkdev *bd, uint64_t blk_id, const void *buf,
		     uint32_t blk_cnt);
};

struct chrdev *chrdev_alloc(void);
void chrdev_free(struct chrdev *cd);
int chrdev_register(struct chrdev *cd, dev_t dev);
void chrdev_unregister(struct chrdev *cd);
struct chrdev *chrdev_get(dev_t dev);

struct blkdev *blkdev_alloc(void);
void blkdev_free(struct blkdev *bd);
int blkdev_register(struct blkdev *bd, dev_t dev);
void blkdev_unregister(struct blkdev *bd);
struct blkdev *blkdev_get(dev_t dev);

void dev_init(void);

#endif
