#include <brk/asm.h>
#include <brk/assert.h>
#include <brk/console.h>
#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/limits.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/macros.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <brk/virtio_blk.h>

struct list_head cdev_list[NR_DEVICES];
static SPINLOCK_DEFINE(cdev_list_lock);
struct list_head bdev_list[NR_DEVICES];
static SPINLOCK_DEFINE(bdev_list_lock);

struct chrdev *chrdev_alloc(void)
{
	struct chrdev *cd = kzalloc(sizeof(*cd));
	if (!cd)
		return NULL;
	cd->ops = kzalloc(sizeof(*cd->ops));
	if (!cd->ops) {
		kfree(cd);
		return NULL;
	}
	return cd;
}

void chrdev_free(struct chrdev *cd)
{
	kfree(cd->ops);
	kfree(cd);
}

static struct chrdev *cdev_get_no_lock(dev_t dev)
{
	struct chrdev *cd = NULL;
	struct list_head *h = &cdev_list[MAJOR(dev)];
	if (list_empty(h))
		return NULL;

	list_for_each_entry(cd, h, list) {
		if (cd->dev == dev)
			return cd;
	}

	return NULL;
}

int chrdev_register(struct chrdev *cd, dev_t dev)
{
	uint32_t major = MAJOR(dev);

	spinlock_acquire(&cdev_list_lock);
	if (major >= NR_DEVICES || cdev_get_no_lock(dev) != NULL) {
		spinlock_release(&cdev_list_lock);
		return -EBUSY;
	}
	cd->dev = dev;
	list_add(&cd->list, &cdev_list[major]);
	spinlock_release(&cdev_list_lock);
	return 0;
}

void chrdev_unregister(struct chrdev *cd)
{
	uint32_t major = MAJOR(cd->dev);
	if (major >= NR_DEVICES)
		return;
	spinlock_acquire(&cdev_list_lock);
	if (cdev_get_no_lock(cd->dev) != NULL)
		list_del(&cd->list);
	spinlock_release(&cdev_list_lock);
}

struct chrdev *chrdev_get(dev_t dev)
{
	struct chrdev *cd = NULL;
	spinlock_acquire(&cdev_list_lock);
	cd = cdev_get_no_lock(dev);
	spinlock_release(&cdev_list_lock);
	return cd;
}

struct blkdev *blkdev_alloc(void)
{
	struct blkdev *bd = kzalloc(sizeof(*bd));
	if (!bd)
		return NULL;
	bd->ops = kzalloc(sizeof(*bd->ops));
	if (!bd->ops) {
		kfree(bd);
		return NULL;
	}
	return bd;
}

void blkdev_free(struct blkdev *bd)
{
	kfree(bd->ops);
	kfree(bd);
}

static struct blkdev *bdev_get_no_lock(dev_t dev)
{
	struct blkdev *bd = NULL;
	struct list_head *h = &bdev_list[MAJOR(dev)];
	if (list_empty(h))
		return NULL;

	list_for_each_entry(bd, h, list) {
		if (bd->dev == dev)
			return bd;
	}

	return NULL;
}

int blkdev_register(struct blkdev *bd, dev_t dev)
{
	uint32_t major = MAJOR(dev);

	spinlock_acquire(&bdev_list_lock);
	if (major >= NR_DEVICES || bdev_get_no_lock(dev) != NULL) {
		spinlock_release(&bdev_list_lock);
		return -1;
	}
	bd->dev = dev;
	list_add(&bd->list, &bdev_list[major]);
	spinlock_release(&bdev_list_lock);
	return 0;
}

void blkdev_unregister(struct blkdev *bd)
{
	uint32_t major = MAJOR(bd->dev);
	if (major >= NR_DEVICES)
		return;
	spinlock_acquire(&bdev_list_lock);
	if (bdev_get_no_lock(bd->dev) != NULL)
		list_del(&bd->list);
	spinlock_release(&bdev_list_lock);
}

struct blkdev *blkdev_get(dev_t dev)
{
	struct blkdev *bd = NULL;
	spinlock_acquire(&bdev_list_lock);
	bd = bdev_get_no_lock(dev);
	spinlock_release(&bdev_list_lock);
	return bd;
}

struct disk0_priv {
	void *buf;
	sleeplock_t lock;
};

static int disk0_read(struct blkdev *bd, uint64_t blk_id, void *buf,
		      uint32_t blk_cnt)
{
	struct disk0_priv *priv = bd->priv;
	uint64_t buf_phys = virt_to_phys((uint64_t)priv->buf);
	int err = 0;

	if (blk_id >= bd->phy_bcnt) {
		log_warn("%s(): Invalid blk_id: %lu, phy_bcnt: %lu\n", __func__,
			 blk_id, bd->phy_bcnt);
		return -ENXIO;
	}

	if (bd->phy_bcnt - blk_id < blk_cnt) {
		log_warn("%s(): Invalid blk_cnt: %u, phy_bcnt: %lu\n", __func__,
			 blk_cnt, bd->phy_bcnt);
		return -ENXIO;
	}

	if (blk_cnt == 0)
		return 0;

	sleeplock_acquire(&priv->lock);
	for (uint32_t i = 0; i < blk_cnt; ++i) {
		err = virtio_blk_read(blk_id, buf_phys, 1);
		if (err) {
			sleeplock_release(&priv->lock);
			return err;
		}
		memcpy(buf, priv->buf, SECTOR_SIZE);
		blk_id += 1;
		buf = (uint8_t *)buf + SECTOR_SIZE;
	}
	sleeplock_release(&priv->lock);

	return 0;
}

static int disk0_write(struct blkdev *bd, uint64_t blk_id, const void *buf,
		       uint32_t blk_cnt)
{
	struct disk0_priv *priv = bd->priv;
	uint64_t buf_phys = virt_to_phys((uint64_t)priv->buf);
	int err = 0;

	sleeplock_acquire(&priv->lock);
	for (uint32_t i = 0; i < blk_cnt; ++i) {
		memcpy(priv->buf, buf, SECTOR_SIZE);
		err = virtio_blk_write(blk_id, buf_phys, 1);
		if (err) {
			sleeplock_release(&priv->lock);
			return err;
		}
		blk_id += 1;
		buf = (const uint8_t *)buf + SECTOR_SIZE;
	}
	sleeplock_release(&priv->lock);

	return 0;
}

void dev_init(void)
{
	struct chrdev *cd;
	struct blkdev *bd;

	for (int i = 0; i < NR_DEVICES; ++i)
		list_init(&cdev_list[i]);
	for (int i = 0; i < NR_DEVICES; ++i)
		list_init(&bdev_list[i]);

	cd = chrdev_alloc();
	assert(cd);
	cd->ops->read = console_read;
	cd->ops->write = console_write;
	chrdev_register(cd, DEV_CONSOLE0);

	bd = blkdev_alloc();
	assert(bd);
	bd->ops->read = disk0_read;
	bd->ops->write = disk0_write;
	bd->phy_bcnt = DISK0_SIZE / SECTOR_SIZE;
	bd->phy_bsize = SECTOR_SIZE;
	struct disk0_priv *priv = kmalloc(sizeof(*priv));
	assert(priv);
	priv->buf = kmalloc(SECTOR_SIZE);
	assert(priv->buf);
	sleeplock_init(&priv->lock, "disk0_buf");
	bd->priv = priv;
	blkdev_register(bd, DEV_DISK0);
}

static int chrdev_open(struct inode *inode, struct file *file)
{
	file->f_op = &chrdev_fops;
	return 0;
}

static ssize_t chrdev_read(struct file *file, char *buf, size_t size,
			   loff_t *pos)
{
	struct inode *ip = file->f_inode;
	int err = 0;
	struct chrdev *cd;
	size_t rcnt = 0;

	sleeplock_acquire(&ip->i_rwsem);

	cd = chrdev_get(ip->i_rdev);
	if (!cd) {
		err = -ENODEV;
		goto unlock_and_out;
	}

	err = cd->ops->read(buf, size, &rcnt);

unlock_and_out:
	sleeplock_release(&ip->i_rwsem);
	if (err)
		return err;
	return rcnt;
}

static ssize_t chrdev_write(struct file *file, const char *buf, size_t size,
			    loff_t *pos)
{
	struct inode *ip = file->f_inode;
	int err = 0;
	struct chrdev *cd;
	size_t wcnt = 0;

	sleeplock_acquire(&ip->i_rwsem);

	cd = chrdev_get(ip->i_rdev);
	if (!cd) {
		err = -ENODEV;
		goto unlock_and_out;
	}

	err = cd->ops->write(buf, size, &wcnt);

unlock_and_out:
	sleeplock_release(&ip->i_rwsem);
	if (err)
		return err;
	return wcnt;
}

static loff_t chrdev_llseek(struct file *file, loff_t offset, int whence)
{
	return 0;
}

static int chrdev_iterate_shared(struct file *file, struct dir_context *ctx)
{
	return 0;
}

static int chrdev_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	return 0;
}

static int chrdev_flush(struct file *file)
{
	return 0;
}

static long chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static int blkdev_open(struct inode *inode, struct file *file)
{
	file->f_op = &blkdev_fops;
	return 0;
}

static ssize_t blkdev_read(struct file *file, char *buf, size_t size,
			   loff_t *pos)
{
	return 0;
}

static ssize_t blkdev_write(struct file *file, const char *buf, size_t size,
			    loff_t *pos)
{
	return 0;
}

static loff_t blkdev_llseek(struct file *file, loff_t offset, int whence)
{
	return 0;
}

static int blkdev_iterate_shared(struct file *file, struct dir_context *ctx)
{
	return 0;
}

static int blkdev_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	return 0;
}

static int blkdev_flush(struct file *file)
{
	return 0;
}

static long blkdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}

const struct file_operations chrdev_fops = {
	.open = chrdev_open,
	.read = chrdev_read,
	.write = chrdev_write,
	.llseek = chrdev_llseek,
	.iterate_shared = chrdev_iterate_shared,
	.fsync = chrdev_fsync,
	.flush = chrdev_flush,
	.ioctl = chrdev_ioctl,
};

const struct file_operations blkdev_fops = {
	.open = blkdev_open,
	.read = blkdev_read,
	.write = blkdev_write,
	.llseek = blkdev_llseek,
	.iterate_shared = blkdev_iterate_shared,
	.fsync = blkdev_fsync,
	.flush = blkdev_flush,
	.ioctl = blkdev_ioctl,
};
