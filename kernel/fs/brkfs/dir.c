#include "brkfs.h"
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/string.h>
#include <uapi/brk/errno.h>
#include <uapi/dirent.h>

static int brkfs_dir_open(struct fs_inode *inode, struct fs_file *file)
{
	file->ops = &brkfs_dir_fops;
	(void)inode;
	return 0;
}

static int brkfs_dir_release(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

static ssize_t brkfs_dir_read(struct fs_file *file, char *buf, size_t size,
			      loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static ssize_t brkfs_dir_write(struct fs_file *file, const char *buf,
			       size_t size, loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static loff_t brkfs_dir_llseek(struct fs_file *file, loff_t offset, int whence)
{
	struct fs_inode *inode = file->inode;
	loff_t new_pos = 0;

	if (whence == SEEK_SET)
		new_pos = offset;
	else if (whence == SEEK_CUR)
		new_pos = file->pos + offset;
	else if (whence == SEEK_END)
		new_pos = inode->size + offset;
	else
		return -EINVAL;
	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int brkfs_dir_iterate_shared(struct fs_file *file,
				    struct fs_dir_iterator *ctx)
{
	struct fs_inode *inode = file->inode;
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	struct brkfs_inode_info *di = inode->private_data;
	size_t bsz = sbi->s_sb.s_blocksize;
	loff_t off = ctx->pos;
	u8 *blk;
	unsigned int bi;
	int err = 0;

	if (!S_ISDIR(inode->mode))
		return -ENOTDIR;

	blk = kmalloc(bsz);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 pblk = di->i_block[bi];
		loff_t blk_base = (loff_t)bi * (loff_t)bsz;
		struct brkfs_dir_entry *ent;
		size_t left;

		if (blk_base >= inode->size)
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
	ctx->pos = inode->size;

out:
	kfree(blk);
	return err;
}

static int brkfs_dir_fsync(struct fs_file *file, loff_t start, loff_t end,
			   int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return -EISDIR;
}

static int brkfs_dir_flush(struct fs_file *file)
{
	(void)file;
	return -EISDIR;
}

static long brkfs_dir_ioctl(struct fs_file *file, unsigned int cmd,
			    unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -EISDIR;
}

static u8 brkfs_mode_to_dt(umode_t mode)
{
	if (S_ISLNK(mode))
		return DT_LNK;
	if (S_ISREG(mode))
		return DT_REG;
	if (S_ISDIR(mode))
		return DT_DIR;
	if (S_ISCHR(mode))
		return DT_CHR;
	if (S_ISBLK(mode))
		return DT_BLK;
	if (S_ISFIFO(mode))
		return DT_FIFO;
	if (S_ISSOCK(mode))
		return DT_SOCK;
	return DT_UNKNOWN;
}

static size_t brkfs_dirent_reclen(unsigned int name_len)
{
	return round_up(offsetof(struct brkfs_dir_entry, name) + name_len, 4u);
}

static int brkfs_dir_ensure_first_block(struct fs_inode *dir)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct brkfs_inode_info *info = dir->private_data;
	u32 bno;
	u8 *blk;
	size_t bs = sbi->s_sb.s_blocksize;
	int err;

	if (dir->size > 0)
		return 0;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;
	err = brkfs_data_alloc(sbi, &bno);
	if (err) {
		kfree(blk);
		return err;
	}
	memset(blk, 0, bs);

	struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)blk;
	e->inode = 0;
	e->name_len = 0;
	e->file_type = 0;
	e->entry_len = (u16)bs;

	info->i_block[0] = bno;
	dir->size = (loff_t)bs;
	err = brkfs_block_write(sbi, bno, blk);
	kfree(blk);
	return err;
}

static int __brkfs_dir_add_in_block(u8 *blk, size_t bs, u32 ino,
				    const char *name, unsigned int name_len,
				    u8 ftype, s32 *walk_off)
{
	u16 n = bs;
	u16 new_min = brkfs_dirent_reclen(name_len);
	struct brkfs_dir_entry *ent = (struct brkfs_dir_entry *)blk;
	struct brkfs_dir_entry *new_ent;

	while (n >= BRKFS_DIR_ENTRY_MIN_LEN) {
		u16 ent_len = ent->entry_len;
		u16 ent_min = brkfs_dirent_reclen(ent->name_len);

		if (ent->inode == 0 && ent_len >= new_min) {
			u16 rest = ent_len - new_min;

			new_ent = ent;
			new_ent->inode = ino;
			new_ent->name_len = name_len;
			new_ent->file_type = ftype;
			new_ent->entry_len = new_min;
			memcpy(new_ent->name, name, name_len);
			if (rest >= BRKFS_DIR_ENTRY_MIN_LEN) {
				struct brkfs_dir_entry *hole =
					(struct brkfs_dir_entry
						 *)((u8 *)new_ent + new_min);

				hole->inode = 0;
				hole->name_len = 0;
				hole->file_type = 0;
				hole->entry_len = rest;
			}
			return 0;
		}
		if (ent->inode != 0 && ent_len - ent_min >= new_min) {
			ent->entry_len = ent_min;
			new_ent =
				(struct brkfs_dir_entry *)((u8 *)ent + ent_min);
			new_ent->entry_len = ent_len - ent_min;
			new_ent->inode = ino;
			new_ent->name_len = name_len;
			new_ent->file_type = ftype;
			memcpy(new_ent->name, name, name_len);
			return 0;
		}

		*walk_off += ent_len;
		ent = (struct brkfs_dir_entry *)((u8 *)ent + ent_len);
		n -= ent_len;
	}

	return -ENOSPC;
}

int brkfs_dir_add(struct fs_inode *dir, u32 child_ino, const char *name,
		  unsigned int name_len, umode_t child_mode)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct brkfs_inode_info *di = dir->private_data;
	size_t bs = sbi->s_sb.s_blocksize;
	u8 ftype = brkfs_mode_to_dt(child_mode);
	u8 *blk;
	unsigned int bi;
	int err;
	s32 walk;

	if (ftype == DT_UNKNOWN)
		return -EINVAL;

	err = brkfs_dir_ensure_first_block(dir);
	if (err)
		return err;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 bno = di->i_block[bi];
		loff_t blk_base = bi * bs;

		if (blk_base >= dir->size)
			break;
		if (bno == 0)
			continue;

		err = brkfs_block_read(sbi, bno, blk);
		if (err)
			goto out;
		walk = blk_base;
		if (__brkfs_dir_add_in_block(blk, bs, child_ino, name, name_len,
					     ftype, &walk) == 0) {
			err = brkfs_block_write(sbi, bno, blk);
			goto out;
		}
	}

	if (bi < BRKFS_DIRECT_BLOCKS) {
		u32 bno;

		err = brkfs_data_alloc(sbi, &bno);
		if (err)
			goto out;
		memset(blk, 0, bs);

		struct brkfs_dir_entry *e = (struct brkfs_dir_entry *)blk;
		e->inode = 0;
		e->name_len = 0;
		e->file_type = 0;
		e->entry_len = bs;

		di->i_block[bi] = bno;
		dir->size = (bi + 1) * bs;
		walk = bi * bs;
		if (__brkfs_dir_add_in_block(blk, bs, child_ino, name, name_len,
					     ftype, &walk) == 0)
			err = brkfs_block_write(sbi, bno, blk);
		else
			err = -ENOMEM;
	} else {
		err = -ENOSPC;
	}

out:
	kfree(blk);
	return err;
}

static int __brkfs_dir_remove_in_block(u8 *blk, size_t bs,
				       unsigned int name_len, const char *name)
{
	struct brkfs_dir_entry *curr = (struct brkfs_dir_entry *)blk;
	struct brkfs_dir_entry *prev = NULL;
	u16 n = bs;
	unsigned int l;

	while (n >= BRKFS_DIR_ENTRY_MIN_LEN) {
		u16 ent_len = curr->entry_len;

		l = name_len < curr->name_len ? name_len : curr->name_len;
		if (curr->inode != 0 && curr->name_len == name_len &&
		    !memcmp(curr->name, name, l)) {
			if (prev)
				prev->entry_len += ent_len;
			else
				curr->inode = 0;
			return 0;
		}

		prev = curr;
		n -= ent_len;
		curr = (struct brkfs_dir_entry *)((u8 *)curr + ent_len);
	}

	return -ENOENT;
}

int brkfs_new_dir_body(struct fs_inode *inode, u32 parent_ino)
{
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	struct brkfs_inode_info *di = inode->private_data;
	size_t bs = sbi->s_sb.s_blocksize;
	u32 self_ino = inode->ino;
	u32 bno;
	u8 *blk;
	struct brkfs_dir_entry *e;
	struct brkfs_dir_entry *e2;
	struct brkfs_dir_entry *hole;
	size_t r1, r2, rest;
	int err;

	if (!di || !S_ISDIR(inode->mode))
		return -EINVAL;
	if (inode->size != 0)
		return 0;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;
	memset(blk, 0, bs);

	r1 = brkfs_dirent_reclen(1);
	r2 = brkfs_dirent_reclen(2);
	if (r1 + r2 + BRKFS_DIR_ENTRY_MIN_LEN > bs) {
		kfree(blk);
		return -EIO;
	}

	e = (struct brkfs_dir_entry *)blk;
	e->inode = self_ino;
	e->name_len = 1;
	e->file_type = DT_DIR;
	e->entry_len = r1;
	e->name[0] = '.';

	e2 = (struct brkfs_dir_entry *)(blk + r1);
	e2->inode = parent_ino;
	e2->name_len = 2;
	e2->file_type = DT_DIR;
	e2->entry_len = r2;
	e2->name[0] = '.';
	e2->name[1] = '.';

	rest = bs - r1 - r2;
	hole = (struct brkfs_dir_entry *)(blk + r1 + r2);
	hole->inode = 0;
	hole->name_len = 0;
	hole->file_type = 0;
	hole->entry_len = rest;

	err = brkfs_data_alloc(sbi, &bno);
	if (err) {
		kfree(blk);
		return err;
	}
	di->i_block[0] = bno;
	inode->size = bs;
	err = brkfs_block_write(sbi, bno, blk);
	kfree(blk);
	return err;
}

int brkfs_dir_remove(struct fs_inode *dir, const char *name,
		     unsigned int name_len)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct brkfs_inode_info *di = dir->private_data;
	size_t bs = sbi->s_sb.s_blocksize;
	u8 *blk;
	unsigned int bi;
	int err = -ENOENT;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 bno = di->i_block[bi];
		loff_t blk_base = bi * bs;

		if (blk_base >= dir->size)
			break;
		if (bno == 0)
			continue;

		err = brkfs_block_read(sbi, bno, blk);
		if (err)
			goto out;
		if (__brkfs_dir_remove_in_block(blk, bs, name_len, name) == 0) {
			err = brkfs_block_write(sbi, bno, blk);
			goto out;
		}
	}

out:
	kfree(blk);
	return err;
}

int brkfs_dir_lookup(struct fs_inode *dir, const char *name,
		     unsigned int name_len, u32 *ino_out, u8 *type_out)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct brkfs_inode_info *di = dir->private_data;
	size_t bs = sbi->s_sb.s_blocksize;
	u8 *blk;
	unsigned int bi;

	if (!S_ISDIR(dir->mode))
		return -ENOTDIR;

	blk = kmalloc(bs);
	if (!blk)
		return -ENOMEM;

	for (bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 bno = di->i_block[bi];
		loff_t blk_base = bi * bs;
		struct brkfs_dir_entry *ent;
		size_t left;

		if (blk_base >= dir->size)
			break;
		if (bno == 0)
			continue;

		int err = brkfs_block_read(sbi, bno, blk);
		if (err) {
			kfree(blk);
			return err;
		}
		ent = (struct brkfs_dir_entry *)blk;
		left = bs;
		while (left >= BRKFS_DIR_ENTRY_MIN_LEN) {
			u16 el = ent->entry_len;

			if (el < BRKFS_DIR_ENTRY_MIN_LEN || el > left) {
				kfree(blk);
				return -EIO;
			}
			if (ent->inode != 0 && ent->name_len == name_len &&
			    !memcmp(ent->name, name, name_len)) {
				*ino_out = ent->inode;
				if (type_out)
					*type_out = ent->file_type;
				kfree(blk);
				return 0;
			}
			ent = (struct brkfs_dir_entry *)((u8 *)ent + el);
			left -= el;
		}
	}

	return -ENOENT;
}

const struct fs_file_ops brkfs_dir_fops = {
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
