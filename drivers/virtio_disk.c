#include <brk/dev.h>
#include <brk/errno.h>
#include <brk/mm.h>
#include <brk/virtio_blk.h>

static int virtio_disk_rw(struct blkdev *bd, u64 blk_id, void *buf, u32 blk_cnt,
			  bool write)
{
	int err;
	u64 buf_phys;

	err = blkdev_check_bounds(bd, blk_id, blk_cnt);
	if (err)
		return err;

	buf_phys = virt_to_phys((u64)buf);
	if (write)
		return virtio_blk_write(blk_id, buf_phys, blk_cnt);
	return virtio_blk_read(blk_id, buf_phys, blk_cnt);
}

static int virtio_disk_read(struct blkdev *bd, u64 blk_id, void *buf,
			    u32 blk_cnt)
{
	return virtio_disk_rw(bd, blk_id, buf, blk_cnt, false);
}

static int virtio_disk_write(struct blkdev *bd, u64 blk_id, const void *buf,
			     u32 blk_cnt)
{
	return virtio_disk_rw(bd, blk_id, (void *)buf, blk_cnt, true);
}

int virtio_disk_init(void)
{
	struct blkdev *bd;
	int err;

	bd = blkdev_alloc();
	if (!bd)
		return -ENOMEM;

	bd->ops.read = virtio_disk_read;
	bd->ops.write = virtio_disk_write;
	bd->phy_bcnt = DISK0_SIZE / SECTOR_SIZE;
	bd->phy_bsize = SECTOR_SIZE;
	bd->priv = NULL;

	err = blkdev_register(bd, DEV_DISK0);
	if (err) {
		blkdev_free(bd);
		return err;
	}
	return 0;
}
