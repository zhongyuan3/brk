#include "brkfs.h"
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/slab.h>

static int brkfs_dir_open(struct fs_inode *inode, struct opened_file *file)
{
	file->f_op = &brkfs_dir_fops;
	(void)inode;
	return 0;
}

static int brkfs_dir_release(struct fs_inode *inode, struct opened_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t brkfs_dir_read(struct opened_file *file, char *buf, usize_t size,
			      loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static ssize_t brkfs_dir_write(struct opened_file *file, const char *buf,
			       usize_t size, loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static loff_t brkfs_dir_llseek(struct opened_file *file, loff_t offset,
			       int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -EISDIR;
}

static int brkfs_dir_iterate_shared(struct opened_file *file,
				    struct fs_dir_iterator *ctx)
{
	struct fs_inode *inode = file->f_inode;
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct brkfs_inode_info *di = inode->i_private;
	usize_t bsz = sbi->s_sb.s_blocksize;
	loff_t off = ctx->pos;
	u8 *blk;
	unsigned int bi;
	int err = 0;

	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;

	blk = kmalloc(bsz);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 pblk = di->i_block[bi];
		loff_t blk_base = (loff_t)bi * (loff_t)bsz;
		struct brkfs_dir_entry *ent;
		usize_t left;

		if (blk_base >= inode->i_size)
			break;
		if (pblk == 0)
			continue;

		err = brkfs_block_read(sbi, pblk, blk);
		if (err)
			break;

		ent = (struct brkfs_dir_entry *)blk;
		left = bsz;
		while (left >= BRKFS_DIR_ENTRY_MIN_LEN) {
			u16 el = ent->entry_len;

			if (el < BRKFS_DIR_ENTRY_MIN_LEN || el > left) {
				err = -EIO;
				goto out;
			}
			if (ent->inode != 0) {
				loff_t entry_off =
					blk_base + (loff_t)((u8 *)ent - blk);

				if (entry_off >= off) {
					if (!ctx->actor(ctx, ent->name,
							ent->name_len,
							entry_off, ent->inode,
							ent->file_type)) {
						ctx->pos = entry_off;
						goto out;
					}
				}
			}
			ent = (struct brkfs_dir_entry *)((u8 *)ent + el);
			left -= el;
		}
	}
	ctx->pos = inode->i_size;

out:
	kfree(blk);
	return err;
}

static int brkfs_dir_fsync(struct opened_file *file, loff_t start, loff_t end,
			   int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return -EISDIR;
}

static int brkfs_dir_flush(struct opened_file *file)
{
	(void)file;
	return -EISDIR;
}

static long brkfs_dir_ioctl(struct opened_file *file, unsigned int cmd,
			    unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -EISDIR;
}

const struct opened_file_ops brkfs_dir_fops = {
	.open = brkfs_dir_open,
	.release = brkfs_dir_release,
	.read = brkfs_dir_read,
	.write = brkfs_dir_write,
	.llseek = brkfs_dir_llseek,
	.iterate_shared = brkfs_dir_iterate_shared,
	.fsync = brkfs_dir_fsync,
	.flush = brkfs_dir_flush,
	.ioctl = brkfs_dir_ioctl,
};
