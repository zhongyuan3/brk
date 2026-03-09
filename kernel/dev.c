#include <aosd/asm.h>
#include <aosd/assert.h>
#include <aosd/console.h>
#include <aosd/dcache.h>
#include <aosd/dev.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/macros.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/types.h>
#include <aosd/virtio_blk.h>

struct list_head cdev_list[NR_DEVICES];
static spinlock_define(cdev_list_lock);
struct list_head bdev_list[NR_DEVICES];
static spinlock_define(bdev_list_lock);

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
		list_init_head(&cdev_list[i]);
	for (int i = 0; i < NR_DEVICES; ++i)
		list_init_head(&bdev_list[i]);

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

static int chrdev_fread(struct file *file, void *buf, size_t n, off_t *offset,
			size_t *rcnt)
{
	struct inode *ip = file->f_inode;
	int ret = 0;
	struct chrdev *cd;

	sleeplock_acquire(&ip->i_lock);

	cd = chrdev_get(ip->i_rdev);
	if (!cd) {
		ret = -ENODEV;
		goto unlock_and_out;
	}

	ret = cd->ops->read(buf, n, rcnt);

unlock_and_out:
	sleeplock_release(&ip->i_lock);
	return ret;
}

static int chrdev_fwrite(struct file *file, const void *buf, size_t n,
			 off_t *offset, size_t *wcnt)
{
	struct inode *ip = file->f_inode;
	int ret = 0;
	struct chrdev *cd;

	sleeplock_acquire(&ip->i_lock);

	cd = chrdev_get(ip->i_rdev);
	if (!cd) {
		ret = -ENODEV;
		goto unlock_and_out;
	}

	ret = cd->ops->write(buf, n, wcnt);

unlock_and_out:
	sleeplock_release(&ip->i_lock);
	return ret;
}

static off_t chrdev_fseek(struct file *file, off_t offset, int whence)
{
	return -EOPNOTSUPP;
}

static int chrdev_fstat(struct file *file, struct stat *st)
{
	if (!file->f_inode)
		return -EINVAL;

	memset(st, 0, sizeof(*st));

	sleeplock_acquire(&file->f_inode->i_lock);
	st->st_dev = file->f_inode->i_sb->s_dev;
	st->st_ino = file->f_inode->i_num;
	st->st_mode = file->f_inode->i_mode;
	st->st_nlink = file->f_inode->i_links;
	st->st_rdev = file->f_inode->i_rdev;
	st->st_size = file->f_inode->i_size;
	st->st_blksize = file->f_inode->i_sb->s_block_size;
	st->st_blocks = file->f_inode->i_size / st->st_blksize;
	sleeplock_release(&file->f_inode->i_lock);

	return 0;
}

static int chrdev_fopen(struct file *file, struct dentry *dentry, int flags)
{
	struct inode *inode = dentry->d_inode;
	sleeplock_acquire(&inode->i_lock);
	if (!(inode->i_mode & S_IFCHR) && !(inode->i_mode & S_IFBLK)) {
		sleeplock_release(&inode->i_lock);
		return -EOPNOTSUPP;
	}
	file->f_dev = inode->i_rdev;
	file->f_ops = &chrdev_fops;
	file->f_inode = inode_dup(inode);
	file->f_dentry = dentry_dup(dentry);
	sleeplock_release(&inode->i_lock);
	return 0;
}

static int chrdev_ftruncate(struct file *file, off_t len)
{
	return -EOPNOTSUPP;
}

static int blkdev_fread(struct file *file, void *buf, size_t n, off_t *offset,
			size_t *rcnt)
{
	return -EOPNOTSUPP;
}

static int blkdev_fwrite(struct file *file, const void *buf, size_t n,
			 off_t *offset, size_t *wcnt)
{
	return -EOPNOTSUPP;
}

static off_t blkdev_fseek(struct file *file, off_t offset, int whence)
{
	return -EOPNOTSUPP;
}

static int blkdev_fstat(struct file *file, struct stat *st)
{
	return -EOPNOTSUPP;
}

static int blkdev_fopen(struct file *file, struct dentry *dentry, int flags)
{
	return -EOPNOTSUPP;
}

static int blkdev_ftruncate(struct file *file, off_t len)
{
	return -EOPNOTSUPP;
}

struct file_operations chrdev_fops = {
	.read = chrdev_fread,
	.write = chrdev_fwrite,
	.seek = chrdev_fseek,
	.stat = chrdev_fstat,
	.open = chrdev_fopen,
	.truncate = chrdev_ftruncate,
};

struct file_operations blkdev_fops = {
	.read = blkdev_fread,
	.write = blkdev_fwrite,
	.seek = blkdev_fseek,
	.stat = blkdev_fstat,
	.open = blkdev_fopen,
	.truncate = blkdev_ftruncate,
};
