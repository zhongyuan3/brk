#include <brk/asm.h>
#include <brk/assert.h>
#include <brk/bitmap.h>
#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/limits.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/tty.h>
#include <brk/types.h>
#include <brk/virtio_blk.h>

struct list_head cdev_list[BRK_MAJOR_MAX];
static SPINLOCK_DEFINE(cdev_list_lock);
struct list_head bdev_list[BRK_MAJOR_MAX];
static SPINLOCK_DEFINE(bdev_list_lock);

/*
 * Set by chrdev_alloc_major / blkdev_alloc_major; cleared when the last
 * device on that major unregisters, or by *_free_major on an empty major.
 */
static BITMAP_DECLARE(cdev_major_pooled, BRK_MAJOR_MAX);
static BITMAP_DECLARE(bdev_major_pooled, BRK_MAJOR_MAX);

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
	unsigned major = MAJOR(dev);

	if (major >= BRK_MAJOR_MAX)
		return NULL;

	if (list_empty(&cdev_list[major]))
		return NULL;

	list_for_each_entry(cd, &cdev_list[major], list) {
		if (cd->dev == dev)
			return cd;
	}

	return NULL;
}

static void cdev_maybe_clear_pool(unsigned major)
{
	if (major >= BRK_MAJOR_MAX)
		return;
	if (list_empty(&cdev_list[major]))
		bitmap_clear_bit(cdev_major_pooled, major);
}

int chrdev_register(struct chrdev *cd, dev_t dev)
{
	unsigned major = MAJOR(dev);
	unsigned minor = MINOR(dev);

	if (!IS_CHRDEV(dev))
		return -EINVAL;
	if (major >= BRK_MAJOR_MAX || minor >= BRK_MINOR_MAX)
		return -EINVAL;

	spinlock_acquire(&cdev_list_lock);
	if (cdev_get_no_lock(dev) != NULL) {
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
	unsigned major = MAJOR(cd->dev);

	if (major >= BRK_MAJOR_MAX)
		return;
	spinlock_acquire(&cdev_list_lock);
	if (cdev_get_no_lock(cd->dev) != NULL)
		list_del(&cd->list);
	cdev_maybe_clear_pool(major);
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

int chrdev_alloc_major(unsigned *major_out)
{
	unsigned m;

	spinlock_acquire(&cdev_list_lock);
	for (m = BRK_DEV_FIRST_DYNAMIC_MAJOR; m < BRK_MAJOR_MAX; m++) {
		if (!bitmap_test_bit(cdev_major_pooled, m) &&
		    list_empty(&cdev_list[m])) {
			bitmap_set_bit(cdev_major_pooled, m);
			*major_out = m;
			spinlock_release(&cdev_list_lock);
			return 0;
		}
	}
	spinlock_release(&cdev_list_lock);
	return -ENOMEM;
}

void chrdev_free_major(unsigned major)
{
	if (major >= BRK_MAJOR_MAX)
		return;
	spinlock_acquire(&cdev_list_lock);
	if (!list_empty(&cdev_list[major])) {
		spinlock_release(&cdev_list_lock);
		return;
	}
	bitmap_clear_bit(cdev_major_pooled, major);
	spinlock_release(&cdev_list_lock);
}

int chrdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	unsigned n;

	if (major >= BRK_MAJOR_MAX || !minor_out)
		return -EINVAL;

	spinlock_acquire(&cdev_list_lock);
	for (n = 0; n < BRK_DEV_ALLOC_MINOR_SCAN; n++) {
		dev_t dev = MKDEV(CHRDEV, major, n);
		if (!cdev_get_no_lock(dev)) {
			*minor_out = n;
			spinlock_release(&cdev_list_lock);
			return 0;
		}
	}
	spinlock_release(&cdev_list_lock);
	return -ENOMEM;
}

int chrdev_alloc_devnum(dev_t *dev_out)
{
	unsigned major, minor;

	if (!dev_out)
		return -EINVAL;

	spinlock_acquire(&cdev_list_lock);
	for (major = BRK_DEV_FIRST_DYNAMIC_MAJOR; major < BRK_MAJOR_MAX;
	     major++) {
		for (minor = 0; minor < BRK_DEV_ALLOC_MINOR_SCAN; minor++) {
			dev_t dev = MKDEV(CHRDEV, major, minor);
			if (!cdev_get_no_lock(dev)) {
				*dev_out = dev;
				spinlock_release(&cdev_list_lock);
				return 0;
			}
		}
	}
	spinlock_release(&cdev_list_lock);
	return -ENOMEM;
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
	unsigned major = MAJOR(dev);

	if (major >= BRK_MAJOR_MAX)
		return NULL;

	if (list_empty(&bdev_list[major]))
		return NULL;

	list_for_each_entry(bd, &bdev_list[major], list) {
		if (bd->dev == dev)
			return bd;
	}

	return NULL;
}

static void bdev_maybe_clear_pool(unsigned major)
{
	if (major >= BRK_MAJOR_MAX)
		return;
	if (list_empty(&bdev_list[major]))
		bitmap_clear_bit(bdev_major_pooled, major);
}

int blkdev_register(struct blkdev *bd, dev_t dev)
{
	unsigned major = MAJOR(dev);
	unsigned minor = MINOR(dev);

	if (!IS_BLKDEV(dev))
		return -EINVAL;
	if (major >= BRK_MAJOR_MAX || minor >= BRK_MINOR_MAX)
		return -EINVAL;

	spinlock_acquire(&bdev_list_lock);
	if (bdev_get_no_lock(dev) != NULL) {
		spinlock_release(&bdev_list_lock);
		return -EBUSY;
	}
	bd->dev = dev;
	list_add(&bd->list, &bdev_list[major]);
	spinlock_release(&bdev_list_lock);
	return 0;
}

void blkdev_unregister(struct blkdev *bd)
{
	unsigned major = MAJOR(bd->dev);

	if (major >= BRK_MAJOR_MAX)
		return;
	spinlock_acquire(&bdev_list_lock);
	if (bdev_get_no_lock(bd->dev) != NULL)
		list_del(&bd->list);
	bdev_maybe_clear_pool(major);
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

int blkdev_alloc_major(unsigned *major_out)
{
	unsigned m;

	spinlock_acquire(&bdev_list_lock);
	for (m = BRK_DEV_FIRST_DYNAMIC_MAJOR; m < BRK_MAJOR_MAX; m++) {
		if (!bitmap_test_bit(bdev_major_pooled, m) &&
		    list_empty(&bdev_list[m])) {
			bitmap_set_bit(bdev_major_pooled, m);
			*major_out = m;
			spinlock_release(&bdev_list_lock);
			return 0;
		}
	}
	spinlock_release(&bdev_list_lock);
	return -ENOMEM;
}

void blkdev_free_major(unsigned major)
{
	if (major >= BRK_MAJOR_MAX)
		return;
	spinlock_acquire(&bdev_list_lock);
	if (!list_empty(&bdev_list[major])) {
		spinlock_release(&bdev_list_lock);
		return;
	}
	bitmap_clear_bit(bdev_major_pooled, major);
	spinlock_release(&bdev_list_lock);
}

int blkdev_alloc_minor(unsigned major, unsigned *minor_out)
{
	unsigned n;

	if (major >= BRK_MAJOR_MAX || !minor_out)
		return -EINVAL;

	spinlock_acquire(&bdev_list_lock);
	for (n = 0; n < BRK_DEV_ALLOC_MINOR_SCAN; n++) {
		dev_t dev = MKDEV(BLKDEV, major, n);
		if (!bdev_get_no_lock(dev)) {
			*minor_out = n;
			spinlock_release(&bdev_list_lock);
			return 0;
		}
	}
	spinlock_release(&bdev_list_lock);
	return -ENOMEM;
}

int blkdev_alloc_devnum(dev_t *dev_out)
{
	unsigned major, minor;

	if (!dev_out)
		return -EINVAL;

	spinlock_acquire(&bdev_list_lock);
	for (major = BRK_DEV_FIRST_DYNAMIC_MAJOR; major < BRK_MAJOR_MAX;
	     major++) {
		for (minor = 0; minor < BRK_DEV_ALLOC_MINOR_SCAN; minor++) {
			dev_t dev = MKDEV(BLKDEV, major, minor);
			if (!bdev_get_no_lock(dev)) {
				*dev_out = dev;
				spinlock_release(&bdev_list_lock);
				return 0;
			}
		}
	}
	spinlock_release(&bdev_list_lock);
	return -ENOMEM;
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
		klog_warn("%s(): Invalid blk_id: %lu, phy_bcnt: %lu\n", __func__,
			 blk_id, bd->phy_bcnt);
		return -ENXIO;
	}

	if (bd->phy_bcnt - blk_id < blk_cnt) {
		klog_warn("%s(): Invalid blk_cnt: %u, phy_bcnt: %lu\n", __func__,
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

static int dev_console0_read(struct file *file, char *buf, size_t n,
			     size_t *read)
{
	return tty_read(tty_boot(), file, buf, n, read);
}

static int dev_console0_write(struct file *file, const char *buf, size_t n,
			      size_t *written)
{
	(void)file;
	return tty_write(tty_boot(), buf, n, written);
}

static long dev_console0_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	return tty_ioctl(tty_boot(), file, cmd, arg);
}

void dev_init(void)
{
	struct chrdev *cd;
	struct blkdev *bd;
	unsigned i;

	for (i = 0; i < BRK_MAJOR_MAX; ++i) {
		list_init(&cdev_list[i]);
		list_init(&bdev_list[i]);
	}

	cd = chrdev_alloc();
	ASSERT(cd);
	cd->ops->read = dev_console0_read;
	cd->ops->write = dev_console0_write;
	cd->ops->ioctl = dev_console0_ioctl;
	ASSERT(chrdev_register(cd, DEV_CONSOLE0) == 0);

	bd = blkdev_alloc();
	ASSERT(bd);
	bd->ops->read = disk0_read;
	bd->ops->write = disk0_write;
	bd->phy_bcnt = DISK0_SIZE / SECTOR_SIZE;
	bd->phy_bsize = SECTOR_SIZE;
	struct disk0_priv *priv = kmalloc(sizeof(*priv));
	ASSERT(priv);
	priv->buf = kmalloc(SECTOR_SIZE);
	ASSERT(priv->buf);
	sleeplock_init(&priv->lock, "disk0_buf");
	bd->priv = priv;
	ASSERT(blkdev_register(bd, DEV_DISK0) == 0);
}

static int chrdev_open(struct inode *inode, struct file *file)
{
	file->f_op = &chrdev_fops;
	if (inode->i_rdev == DEV_CONSOLE0) {
		struct tty_file_priv *priv;

		priv = tty_file_priv_create();
		if (!priv)
			return -ENOMEM;
		file->private_data = priv;
	}
	return 0;
}

static int chrdev_release(struct inode *inode, struct file *file)
{
	if (inode->i_rdev == DEV_CONSOLE0 && file->private_data) {
		tty_file_priv_destroy(file->private_data);
		file->private_data = NULL;
	}
	return 0;
}

static ssize_t chrdev_read(struct file *file, char *buf, size_t size,
			   loff_t *pos)
{
	(void)pos;
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

	err = cd->ops->read(file, buf, size, &rcnt);

unlock_and_out:
	sleeplock_release(&ip->i_rwsem);
	if (err)
		return err;
	return rcnt;
}

static ssize_t chrdev_write(struct file *file, const char *buf, size_t size,
			    loff_t *pos)
{
	(void)pos;
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

	err = cd->ops->write(file, buf, size, &wcnt);

unlock_and_out:
	sleeplock_release(&ip->i_rwsem);
	if (err)
		return err;
	return wcnt;
}

static loff_t chrdev_llseek(struct file *file, loff_t offset, int whence)
{
	(void)whence;
	(void)offset;
	(void)file;
	return 0;
}

static int chrdev_iterate_shared(struct file *file, struct dir_context *ctx)
{
	(void)file;
	(void)ctx;
	return 0;
}

static int chrdev_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int chrdev_flush(struct file *file)
{
	(void)file;
	return 0;
}

static long chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *ip = file->f_inode;
	struct chrdev *cd;
	long ret = -ENODEV;

	sleeplock_acquire(&ip->i_rwsem);
	cd = chrdev_get(ip->i_rdev);
	if (cd) {
		if (cd->ops->ioctl)
			ret = cd->ops->ioctl(file, cmd, arg);
		else
			ret = -ENOTTY;
	}
	sleeplock_release(&ip->i_rwsem);
	return ret;
}

static int blkdev_open(struct inode *inode, struct file *file)
{
	(void)inode;
	file->f_op = &blkdev_fops;
	return 0;
}

static ssize_t blkdev_read(struct file *file, char *buf, size_t size,
			   loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return 0;
}

static ssize_t blkdev_write(struct file *file, const char *buf, size_t size,
			    loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return 0;
}

static loff_t blkdev_llseek(struct file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return 0;
}

static int blkdev_iterate_shared(struct file *file, struct dir_context *ctx)
{
	(void)file;
	(void)ctx;
	return 0;
}

static int blkdev_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int blkdev_flush(struct file *file)
{
	(void)file;
	return 0;
}

static long blkdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return 0;
}

const struct file_operations chrdev_fops = {
	.open = chrdev_open,
	.release = chrdev_release,
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
