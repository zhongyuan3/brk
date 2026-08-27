#include "brkfs.h"
#include <brk/base/error.h>
#include <brk/base/kernel.h>
#include <brk/base/types.h>
#include <brk/fs/dcache.h>
#include <brk/fs/fs.h>
#include <brk/lib/string.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/pagecache.h>
#include <brk/printk/printk.h>
#include <brk/time/ktime.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/stat.h>

#define ATTR_MODE (1u << 0)
#define ATTR_SIZE (1u << 3)

static void brkfs_inode_attach_ops(struct fs_inode *inode)
{
	inode->ops = &brkfs_iops;
	if (S_ISDIR(inode->mode))
		inode->fops = &brkfs_dir_fops;
	else if (S_ISREG(inode->mode))
		inode->fops = &brkfs_file_fops;
	else if (S_ISLNK(inode->mode))
		inode->fops = &brkfs_file_fops;
	else if (S_ISCHR(inode->mode))
		inode->fops = &chrdev_fops;
	else if (S_ISBLK(inode->mode))
		inode->fops = &blkdev_fops;
	else
		inode->fops = &brkfs_file_fops;
}

static int brkfs_init_loaded_inode(struct brkfs_sb_info *sbi,
				   struct fs_inode *inode)
{
	int err = brkfs_inode_read(sbi, inode);

	if (err)
		return err;
	brkfs_inode_attach_ops(inode);
	/*
	 * Regular files and symlinks store their data through the same block
	 * structure and are read/written through the page cache. Directories
	 * and device nodes use bespoke paths and do not need a mapping.
	 */
	if (S_ISREG(inode->mode) || S_ISLNK(inode->mode)) {
		err = fs_inode_attach_page_cache(inode, &brkfs_file_pc_ops);
		if (err)
			return err;
	}
	fs_inode_unlock_new(inode);
	return 0;
}

static struct fs_dentry *
brkfs_lookup(struct fs_inode *dir, struct fs_dentry *dentry, unsigned int flags)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_super_block *sb = dir->sb;
	struct fs_inode *inode;
	uint32_t ino;
	uint8_t type;
	int err;

	(void)flags;

	if (!S_ISDIR(dir->mode))
		return ERR_PTR(-ENOTDIR);

	err = brkfs_dir_lookup(dir, dentry->name.name, dentry->name.len, &ino,
			       &type);
	if (err == -ENOENT)
		return NULL;
	if (err)
		return ERR_PTR(err);

	inode = fs_inode_get_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	spinlock_acquire(&inode->lock);
	bool is_new = (inode->state & I_NEW) != 0;
	spinlock_release(&inode->lock);
	if (is_new) {
		err = brkfs_init_loaded_inode(sbi, inode);
		if (err) {
			fs_inode_put(inode);
			return ERR_PTR(err);
		}
	}

	return fs_dentry_splice_alias(inode, dentry);
}

static int brkfs_create(struct fs_inode *dir, struct fs_dentry *dentry,
			umode_t mode, bool excl)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_super_block *sb = dir->sb;
	struct fs_inode *inode = NULL;
	uint32_t ino = 0;
	int err;

	err = brkfs_dir_lookup(dir, dentry->name.name, dentry->name.len, &ino,
			       NULL);
	if (!err && excl) {
		err = -EEXIST;
		goto out;
	}
	if (err != -ENOENT) {
		goto out;
	}

	err = brkfs_inode_alloc(sbi, &ino);
	if (err)
		goto out;
	err = brkfs_disk_inode_init(sbi, ino, S_IFREG | mode, 1, 0);
	if (err)
		goto undo_alloc;

	inode = fs_inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->name.name, dentry->name.len,
			    S_IFREG | mode);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	fs_dentry_instantiate(dentry, inode);
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err)
		goto out;
	err = brkfs_inode_write(sbi, inode);
	goto out;

undo_alloc:
	brkfs_inode_free(sbi, ino);
out:
	return err;
}

static int brkfs_link(struct fs_dentry *old_dentry, struct fs_inode *dir,
		      struct fs_dentry *new_dentry)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_inode *old_inode = old_dentry->inode;
	int err;

	err = brkfs_dir_add(dir, (uint32_t)old_inode->ino,
			    new_dentry->name.name, new_dentry->name.len,
			    old_inode->mode);
	if (err)
		goto out;
	fs_inode_get(old_inode);
	old_inode->nlink++;
	inode_touch_ctime(old_inode);
	err = brkfs_inode_write(sbi, old_inode);
	if (err) {
		old_inode->nlink--;
		fs_inode_put(old_inode);
		goto out;
	}
	fs_dentry_instantiate(new_dentry, old_inode);
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
out:
	return err;
}

static int brkfs_unlink(struct fs_inode *dir, struct fs_dentry *dentry)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_inode *inode = dentry->inode;
	int err;

	err = brkfs_dir_remove(dir, dentry->name.name, dentry->name.len);
	if (err)
		return err;
	inode->nlink--;
	inode_touch_ctime(inode);
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err)
		return err;
	return brkfs_inode_write(sbi, inode);
	/*
	 * Do NOT fs_inode_put(inode) here: the inode reference belongs to the
	 * dentry. The eventual fs_dentry_put() will drop it and trigger
	 * eviction (which now also runs the page cache teardown).
	 */
}

static int brkfs_symlink(struct fs_inode *dir, struct fs_dentry *dentry,
			 const char *symname)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_super_block *sb = dir->sb;
	struct fs_inode *inode = NULL;
	uint32_t ino = 0;
	size_t len = strlen(symname);
	loff_t pos = 0;
	size_t w = 0;
	int err;

	err = brkfs_inode_alloc(sbi, &ino);
	if (err)
		goto out;
	err = brkfs_disk_inode_init(sbi, ino, S_IFLNK | 0777, 1, 0);
	if (err)
		goto undo_alloc;

	inode = fs_inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	err = brkfs_file_write_at(inode, &pos, symname, len, &w);
	if (err || w != len) {
		fs_inode_put(inode);
		err = err ? err : -EIO;
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->name.name, dentry->name.len,
			    S_IFLNK | 0777);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	fs_dentry_instantiate(dentry, inode);
	err = brkfs_inode_write(sbi, inode);
	if (err)
		goto out;
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	goto out;

undo_alloc:
	brkfs_inode_free(sbi, ino);
out:
	return err;
}

static int brkfs_readlink(struct fs_dentry *dentry, char *buf, int bufsiz)
{
	struct fs_inode *inode = dentry->inode;
	loff_t pos = 0;
	size_t rd = 0;
	int err;

	if (!S_ISLNK(inode->mode))
		return -EINVAL;
	if (bufsiz <= 0)
		return -EINVAL;
	err = brkfs_file_read_at(inode, &pos, buf, (size_t)(bufsiz - 1), &rd);
	if (err)
		return err;
	buf[rd] = '\0';
	return (int)rd;
}

static int brkfs_mkdir(struct fs_inode *dir, struct fs_dentry *dentry,
		       umode_t mode)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_super_block *sb = dir->sb;
	struct fs_inode *inode = NULL;
	uint32_t ino = 0;
	int err;

	err = brkfs_inode_alloc(sbi, &ino);
	if (err)
		goto out;
	err = brkfs_disk_inode_init(sbi, ino, (umode_t)(S_IFDIR | mode), 2, 0);
	if (err)
		goto undo_alloc;

	inode = fs_inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	err = brkfs_new_dir_body(inode, (uint32_t)dir->ino);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}
	err = brkfs_inode_write(sbi, inode);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->name.name, dentry->name.len,
			    (umode_t)(S_IFDIR | mode));
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	dir->nlink++;
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err) {
		dir->nlink--;
		fs_inode_put(inode);
		goto out;
	}

	fs_dentry_instantiate(dentry, inode);
	err = 0;
	goto out;

undo_alloc:
	brkfs_inode_free(sbi, ino);
out:
	return err;
}

static int brkfs_rmdir(struct fs_inode *dir, struct fs_dentry *dentry)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_inode *inode = dentry->inode;
	int err;

	if (!S_ISDIR(inode->mode))
		return -ENOTDIR;
	if (inode->nlink > 2)
		return -ENOTEMPTY;

	err = brkfs_dir_remove(dir, dentry->name.name, dentry->name.len);
	if (err)
		return err;
	dir->nlink--;
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err)
		return err;
	inode->nlink = 0;
	inode_touch_ctime(inode);
	return brkfs_inode_write(sbi, inode);
	/*
	 * Do NOT fs_inode_put(inode) here. The inode reference belongs to the
	 * dentry; fs_dentry_put() will trigger the final eviction.
	 */
}

static int brkfs_rename(struct fs_inode *old_dir, struct fs_dentry *old_dentry,
			struct fs_inode *new_dir, struct fs_dentry *new_dentry,
			unsigned int flags)
{
	(void)old_dir;
	(void)old_dentry;
	(void)new_dir;
	(void)new_dentry;
	(void)flags;
	return -EOPNOTSUPP;
}

static int brkfs_mknod(struct fs_inode *dir, struct fs_dentry *dentry,
		       umode_t mode, dev_t dev)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_super_block *sb = dir->sb;
	struct fs_inode *inode;
	uint32_t ino;
	int err;

	err = brkfs_inode_alloc(sbi, &ino);
	if (err)
		goto out;
	err = brkfs_disk_inode_init(sbi, ino, mode, 1, dev);
	if (err)
		goto undo_alloc;

	inode = fs_inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->name.name, dentry->name.len,
			    mode);
	if (err) {
		fs_inode_put(inode);
		goto out;
	}

	fs_dentry_instantiate(dentry, inode);
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err)
		goto out;
	err = brkfs_inode_write(sbi, inode);
	goto out;

undo_alloc:
	brkfs_inode_free(sbi, ino);
out:
	return err;
}

static int brkfs_getattr(const struct fs_path *path, struct stat *stat,
			 uint32_t mask, unsigned int flags)
{
	struct fs_inode *inode = path->dentry->inode;

	(void)mask;
	(void)flags;

	memset(stat, 0, sizeof(*stat));
	stat->st_ino = inode->ino;
	stat->st_mode = inode->mode;
	stat->st_nlink = inode->nlink;
	stat->st_rdev = inode->rdev;
	stat->st_size = inode->size;
	stat->st_blksize = (int)inode->sb->block_size;
	stat->st_blocks = div_ceil(inode->size, inode->sb->block_size);
	inode_times_to_stat(inode, stat);
	return 0;
}

static int brkfs_setattr(struct fs_dentry *dentry, struct fs_iattr *attr)
{
	struct fs_inode *inode = dentry->inode;
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	int err = 0;

	if (attr->valid & ATTR_MODE) {
		inode->mode = attr->mode;
		inode_touch_ctime(inode);
	}
	if (attr->valid & ATTR_SIZE) {
		/*
		 * Shrink: drop cache pages past the new EOF, zero the partial
		 * tail page, flush dirty data, then free on-disk blocks. The
		 * inode rwsem excludes concurrent generic_file I/O.
		 */
		sleeplock_acquire(&inode->rwsem);
		if (inode->mapping) {
			err = truncate_inode_pages(inode->mapping, attr->size);
			if (!err)
				err = page_cache_flush(inode->mapping);
		}
		if (!err)
			err = brkfs_truncate_inode_blocks(inode, attr->size);
		sleeplock_release(&inode->rwsem);
		if (err)
			goto out;
		inode_touch_mtime_ctime(inode);
	}
	err = brkfs_inode_write(sbi, inode);
out:
	return err;
}

void brkfs_inode_setup_ops(struct fs_inode *inode)
{
	brkfs_inode_attach_ops(inode);
}

int brkfs_inode_read(struct brkfs_sb_info *sbi, struct fs_inode *inode)
{
	struct brkfs_inode *disk_i;
	struct brkfs_inode_info *info;
	uint32_t bno;
	int err;
	uint32_t ino = inode->ino;
	uint32_t isize = sbi->s_sb.s_inode_size;
	struct brkfs_block bb;

	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		klog_warn("%s(): Invalid ino: %u\n", __func__, ino);
		return -EINVAL;
	}

	bno = sbi->s_sb.s_inode_table + (ino - 1) / sbi->s_inodes_per_block;

	err = brkfs_get_block(sbi, bno, &bb);
	if (err)
		return err;

	cached_page_lock(bb.cp);

	uint32_t idx = (ino - 1) % sbi->s_inodes_per_block;
	disk_i = (struct brkfs_inode *)((uint8_t *)bb.data + idx * isize);

	info = inode->private_data;

	inode->mode = disk_i->i_mode;
	inode->rdev = disk_i->i_rdev;
	inode->nlink = disk_i->i_nlink;
	inode->size = disk_i->i_size;
	memcpy(info->i_block, disk_i->i_block, sizeof(disk_i->i_block));
	inode->atime.tv_sec = disk_i->i_atime;
	inode->atime.tv_nsec = disk_i->i_atime_nsec;
	inode->mtime.tv_sec = disk_i->i_mtime;
	inode->mtime.tv_nsec = disk_i->i_mtime_nsec;
	inode->ctime.tv_sec = disk_i->i_ctime;
	inode->ctime.tv_nsec = disk_i->i_ctime_nsec;
	inode->uid = disk_i->i_uid;
	inode->gid = disk_i->i_gid;

	cached_page_unlock(bb.cp);
	brkfs_put_block(&bb);
	return 0;
}

int brkfs_inode_write(struct brkfs_sb_info *sbi, struct fs_inode *inode)
{
	uint32_t bno;
	uint32_t ino = inode->ino;
	int err;
	struct brkfs_inode *disk_i;
	struct brkfs_inode_info *info;
	uint32_t isize = sbi->s_sb.s_inode_size;
	struct brkfs_block bb;

	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		klog_warn("%s(): Invalid ino: %u\n", __func__, ino);
		return -EINVAL;
	}

	bno = sbi->s_sb.s_inode_table + (ino - 1) / sbi->s_inodes_per_block;

	err = brkfs_get_block(sbi, bno, &bb);
	if (err)
		return err;

	cached_page_lock(bb.cp);

	uint32_t idx = (ino - 1) % sbi->s_inodes_per_block;
	disk_i = (struct brkfs_inode *)((uint8_t *)bb.data + idx * isize);

	info = inode->private_data;

	disk_i->i_mode = inode->mode;
	disk_i->i_rdev = inode->rdev;
	disk_i->i_nlink = inode->nlink;
	disk_i->i_size = inode->size;
	memcpy(disk_i->i_block, info->i_block, sizeof(disk_i->i_block));
	disk_i->i_atime = inode->atime.tv_sec;
	disk_i->i_atime_nsec = inode->atime.tv_nsec;
	disk_i->i_mtime = inode->mtime.tv_sec;
	disk_i->i_mtime_nsec = inode->mtime.tv_nsec;
	disk_i->i_ctime = inode->ctime.tv_sec;
	disk_i->i_ctime_nsec = inode->ctime.tv_nsec;
	disk_i->i_uid = inode->uid;
	disk_i->i_gid = inode->gid;

	cached_page_mark_dirty(bb.cp);
	cached_page_unlock(bb.cp);
	brkfs_put_block(&bb);
	return 0;
}

int brkfs_inode_alloc(struct brkfs_sb_info *sbi, uint32_t *ino)
{
	uint32_t bit = 0;
	int err;

	err = brkfs_bitmap_alloc(sbi, sbi->s_sb.s_inode_bitmap,
				 sbi->s_sb.s_inodes_count, &bit);
	if (err)
		return err;
	*ino = bit + 1;
	return 0;
}

int brkfs_inode_free(struct brkfs_sb_info *sbi, uint32_t ino)
{
	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		klog_warn("%s(): Invalid ino: %u\n", __func__, ino);
		return -EINVAL;
	}
	ino -= 1;
	return brkfs_bitmap_free(sbi, sbi->s_sb.s_inode_bitmap,
				 sbi->s_sb.s_inodes_count, ino);
}

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, uint32_t ino, umode_t mode,
			  unsigned int nlink, dev_t rdev)
{
	struct fs_inode stub = { 0 };
	struct brkfs_inode_info info;

	memset(&info, 0, sizeof(info));
	stub.ino = ino;
	stub.mode = mode;
	stub.nlink = nlink;
	stub.size = 0;
	stub.rdev = rdev;
	stub.private_data = &info;
	inode_times_set_all_now(&stub);
	return brkfs_inode_write(sbi, &stub);
}

int brkfs_inode_getblk(struct fs_inode *inode, loff_t off, uint32_t *bno,
		       unsigned flags, struct brkfs_sb_info *sbi)
{
	struct brkfs_inode_info *inf = inode->private_data;
	uint32_t *blk_ptrs = inf->i_block;
	bool create = (flags & BRKFS_GETBLK_CREATE) != 0;
	int ret = 0;
	uint32_t bs = sbi->s_sb.s_blocksize;
	uint32_t ptrs_per_blk = sbi->s_sb.s_blocksize / sizeof(uint32_t);
	struct brkfs_block bb;

	uint32_t blk_idx = off / sbi->s_sb.s_blocksize;
	if (blk_idx < BRKFS_DIRECT_BLOCKS) {
		if (blk_ptrs[blk_idx] == 0) {
			if (!create) { /* read of a hole or sparse region */
				*bno = 0;
				return 0;
			}
			uint32_t new_bno = 0;
			ret = brkfs_data_alloc(sbi, &new_bno);
			if (ret != 0)
				return ret;
			blk_ptrs[blk_idx] = new_bno;
			*bno = new_bno;
			return 0;
		}
		*bno = blk_ptrs[blk_idx];
		return 0;
	}

	blk_idx -= BRKFS_DIRECT_BLOCKS;
	if (blk_idx < ptrs_per_blk) {
		uint32_t idb = blk_ptrs[BRKFS_INDIRECT_BLOCK];
		bool new_idb = false;

		if (idb == 0) {
			if (!create) {
				*bno = 0;
				return 0;
			}

			ret = brkfs_data_alloc(sbi, &idb);
			if (ret)
				return ret;

			blk_ptrs[BRKFS_INDIRECT_BLOCK] = idb;
			new_idb = true;
		}

		ret = brkfs_get_block(sbi, idb, &bb);
		if (ret != 0)
			return ret;

		cached_page_lock(bb.cp);

		if (new_idb) {
			memset(bb.data, 0, bs);
			cached_page_mark_dirty(bb.cp);
		}

		uint32_t *idb_ptrs = bb.data;

		if (idb_ptrs[blk_idx] == 0) {
			if (!create) {
				*bno = 0;
				ret = 0;
				goto idb_out_unlock_and_put;
			}

			/* Unlock the inode block before allocating a new data block
			 * to avoid deadlocking with the page cache. */
			cached_page_unlock(bb.cp);
			uint32_t new_bno = 0;
			ret = brkfs_data_alloc(sbi, &new_bno);
			if (ret != 0)
				goto idb_out_put;

			cached_page_lock(bb.cp);
			idb_ptrs[blk_idx] = new_bno;
			cached_page_mark_dirty(bb.cp);
			*bno = new_bno;
		} else {
			*bno = idb_ptrs[blk_idx];
			ret = 0;
		}

idb_out_unlock_and_put:
		cached_page_unlock(bb.cp);
idb_out_put:
		brkfs_put_block(&bb);
		return ret;
	}

	klog_warn("%s(): Double indirect block not implemented\n", __func__);

	return -ENOSPC;
}

int brkfs_truncate_inode_blocks(struct fs_inode *inode, loff_t new_size)
{
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	struct brkfs_inode_info *inf = inode->private_data;
	uint32_t bs = sbi->s_sb.s_blocksize;
	loff_t old_size = inode->size;
	uint32_t old_n;
	uint32_t new_n;
	uint32_t bi;

	if (!inf)
		return -EINVAL;
	if (new_size < 0)
		return -EINVAL;
	if (new_size >= old_size) {
		inode->size = new_size;
		return 0;
	}

	old_n = (uint32_t)((old_size + (loff_t)bs - 1) / (loff_t)bs);
	new_n = (uint32_t)((new_size + (loff_t)bs - 1) / (loff_t)bs);

	for (bi = new_n; bi < old_n && bi < BRKFS_DIRECT_BLOCKS; bi++) {
		if (inf->i_block[bi]) {
			brkfs_data_free(sbi, inf->i_block[bi]);
			inf->i_block[bi] = 0;
		}
	}
	inode->size = new_size;
	return 0;
}

const struct fs_inode_ops brkfs_iops = {
	.lookup = brkfs_lookup,
	.create = brkfs_create,
	.link = brkfs_link,
	.unlink = brkfs_unlink,
	.symlink = brkfs_symlink,
	.readlink = brkfs_readlink,
	.mkdir = brkfs_mkdir,
	.rmdir = brkfs_rmdir,
	.rename = brkfs_rename,
	.mknod = brkfs_mknod,
	.getattr = brkfs_getattr,
	.setattr = brkfs_setattr,
};
