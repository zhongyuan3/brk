#include "brkfs.h"
#include <brk/dcache.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/pagecache.h>
#include <brk/printk.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/stat.h>

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

	if (err) {
		fs_inode_fail_new(inode);
		return err;
	}
	brkfs_inode_attach_ops(inode);
	/*
	 * Regular files and symlinks store their data through the same block
	 * structure and are read/written through the page cache. Directories
	 * and device nodes use bespoke paths and do not need a mapping.
	 */
	if (S_ISREG(inode->mode) || S_ISLNK(inode->mode)) {
		err = fs_inode_attach_page_cache(inode, &brkfs_file_pc_ops);
		if (err) {
			fs_inode_fail_new(inode);
			return err;
		}
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
	u32 ino;
	u8 type;
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

	spin_lock(&inode->lock);
	bool is_new = (inode->state & I_NEW) != 0;
	spin_unlock(&inode->lock);
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
	u32 ino = 0;
	int err;

	(void)excl;

	if (dentry->inode)
		return -EEXIST;

	err = brkfs_dir_lookup(dir, dentry->name.name, dentry->name.len, &ino,
			       NULL);
	/*
	 * The entry already exists (it may have been created after
	 * do_openat() sampled the negative dentry).  Report -EEXIST so the
	 * caller can retry the lookup and open the existing file; returning
	 * success here would hand back a still-negative dentry.
	 */
	if (err == 0) {
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
	u32 ino;
	int err;

	if (S_ISDIR(old_inode->mode))
		return -EPERM;
	if (new_dentry->inode)
		return -EEXIST;

	err = brkfs_dir_lookup(dir, new_dentry->name.name,
			       new_dentry->name.len, &ino, NULL);
	if (err == 0)
		return -EEXIST;
	if (err != -ENOENT)
		return err;

	err = brkfs_dir_add(dir, (u32)old_inode->ino, new_dentry->name.name,
			    new_dentry->name.len, old_inode->mode);
	if (err)
		goto out;
	fs_inode_get(old_inode);
	old_inode->nlink++;
	inode_touch_ctime(old_inode);
	err = brkfs_inode_write(sbi, old_inode);
	if (err) {
		(void)brkfs_dir_remove(dir, new_dentry->name.name,
				       new_dentry->name.len);
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

	if (S_ISDIR(inode->mode))
		return -EISDIR;

	/*
	 * nlink is shared by every hard link, so it must be modified and
	 * written back under the target inode's rwsem.  The VFS only holds
	 * the parent directory's rwsem here.
	 */
	sleeplock_acquire(&inode->rwsem);

	err = brkfs_dir_remove(dir, dentry->name.name, dentry->name.len);
	if (err)
		goto out;

	inode->nlink--;
	inode_touch_ctime(inode);
	inode_touch_mtime_ctime(dir);

	err = brkfs_inode_write(sbi, dir);
	if (err)
		goto out;

	err = brkfs_inode_write(sbi, inode);
out:
	sleeplock_release(&inode->rwsem);
	return err;
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
	u32 ino = 0;
	size_t len = strlen(symname);
	loff_t pos = 0;
	size_t w = 0;
	int err;

	if (dentry->inode)
		return -EEXIST;
	err = brkfs_dir_lookup(dir, dentry->name.name, dentry->name.len, &ino,
			       NULL);
	if (err == 0)
		return -EEXIST;
	if (err != -ENOENT)
		return err;

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
	u32 ino = 0;
	int err;

	if (dentry->inode)
		return -EEXIST;
	err = brkfs_dir_lookup(dir, dentry->name.name, dentry->name.len, &ino,
			       NULL);
	if (err == 0)
		return -EEXIST;
	if (err != -ENOENT)
		return err;

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

	err = brkfs_new_dir_body(inode, (u32)dir->ino);
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

	sleeplock_acquire(&inode->rwsem);

	/*
	 * nlink only counts subdirectories, not ordinary files.  Scan the
	 * actual directory contents instead of using nlink as an emptiness
	 * heuristic.
	 */
	err = brkfs_dir_is_empty(inode);
	if (err < 0)
		goto out;
	if (err == 0) {
		err = -ENOTEMPTY;
		goto out;
	}

	err = brkfs_dir_remove(dir, dentry->name.name, dentry->name.len);
	if (err)
		goto out;

	dir->nlink--;
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err)
		goto out;

	inode->nlink = 0;
	inode_touch_ctime(inode);
	err = brkfs_inode_write(sbi, inode);
out:
	sleeplock_release(&inode->rwsem);
	return err;
	/*
	 * Do NOT fs_inode_put(inode) here. The inode reference belongs to the
	 * dentry; fs_dentry_put() will trigger the final eviction.
	 */
}

static int brkfs_rename(struct fs_inode *old_dir, struct fs_dentry *old_dentry,
			struct fs_inode *new_dir, struct fs_dentry *new_dentry,
			unsigned int flags)
{
	struct brkfs_sb_info *sbi = old_dir->sb->private_data;
	struct fs_inode *inode = old_dentry->inode;
	struct fs_inode *target = new_dentry->inode;
	struct fs_inode *first;
	struct fs_inode *second;
	bool is_dir;
	bool replace = target != NULL;
	u32 ino;
	u32 tino = 0;
	int err;

	if (!inode || !S_ISDIR(old_dir->mode) || !S_ISDIR(new_dir->mode))
		return -ENOTDIR;
	if (replace && (flags & RENAME_NOREPLACE))
		return -EEXIST;

	is_dir = S_ISDIR(inode->mode);
	if (is_dir && old_dir != new_dir)
		return -EXDEV;

	/* Verify the cached view still matches the on-disk directories. */
	err = brkfs_dir_lookup(old_dir, old_dentry->name.name,
			       old_dentry->name.len, &ino, NULL);
	if (err)
		return err;
	if (ino != inode->ino)
		return -ENOENT;

	err = brkfs_dir_lookup(new_dir, new_dentry->name.name,
			       new_dentry->name.len, &tino, NULL);
	if (err == 0) {
		if (!replace || tino != target->ino)
			return -EEXIST;
	} else if (err != -ENOENT) {
		return err;
	} else if (replace) {
		return -ENOENT;
	} else {
		replace = false;
	}

	/*
	 * Lock the moved inode and the replaced inode in a stable order.
	 * Parent directories are already locked by the VFS.
	 */
	first = inode;
	second = target;
	if (second && (uintptr_t)second < (uintptr_t)first) {
		struct fs_inode *tmp = first;

		first = second;
		second = tmp;
	}
	sleeplock_acquire(&first->rwsem);
	if (second)
		sleeplock_acquire(&second->rwsem);

	if (replace && S_ISDIR(target->mode)) {
		err = brkfs_dir_is_empty(target);
		if (err < 0)
			goto out_unlock;
		if (err == 0) {
			err = -ENOTEMPTY;
			goto out_unlock;
		}
	}

	/* Remove the replaced entry first so the new name can reuse space. */
	if (replace) {
		err = brkfs_dir_remove(new_dir, new_dentry->name.name,
				       new_dentry->name.len);
		if (err)
			goto out_unlock;
	}

	err = brkfs_dir_add(new_dir, (u32)inode->ino,
			    new_dentry->name.name, new_dentry->name.len,
			    inode->mode);
	if (err)
		goto rollback_target;

	err = brkfs_dir_remove(old_dir, old_dentry->name.name,
			       old_dentry->name.len);
	if (err) {
		(void)brkfs_dir_remove(new_dir, new_dentry->name.name,
				       new_dentry->name.len);
		goto rollback_target;
	}

	/*
	 * The source and destination are now consistent.  Update link counts
	 * and timestamps.  For a replaced directory, new_dir loses the ".."
	 * link that the removed directory held; adding the renamed directory
	 * restores it (and same-directory directory renames never change the
	 * parent's link count).
	 */
	if (replace) {
		if (S_ISDIR(target->mode)) {
			target->nlink = 0;
			inode_touch_ctime(target);
			err = brkfs_inode_write(sbi, target);
			if (!err) {
				new_dir->nlink--;
				inode_touch_mtime_ctime(new_dir);
			}
		} else {
			target->nlink--;
			inode_touch_ctime(target);
			err = brkfs_inode_write(sbi, target);
		}
		if (err)
			goto out_unlock;
	}

	inode_touch_ctime(inode);
	err = brkfs_inode_write(sbi, inode);
	if (err)
		goto out_unlock;

	inode_touch_mtime_ctime(old_dir);
	err = brkfs_inode_write(sbi, old_dir);
	if (err)
		goto out_unlock;

	if (new_dir != old_dir) {
		inode_touch_mtime_ctime(new_dir);
		err = brkfs_inode_write(sbi, new_dir);
	}

	goto out_unlock;

rollback_target:
	if (replace) {
		int rerr = brkfs_dir_add(new_dir, tino,
					 new_dentry->name.name,
					 new_dentry->name.len,
					 target->mode);

		if (rerr)
			pr_warn("%s(): rollback of %.*s failed: %d\n",
				__func__, (int)new_dentry->name.len,
				new_dentry->name.name, rerr);
	}

out_unlock:
	if (second)
		sleeplock_release(&second->rwsem);
	sleeplock_release(&first->rwsem);
	return err;
}

static int brkfs_mknod(struct fs_inode *dir, struct fs_dentry *dentry,
		       umode_t mode, dev_t dev)
{
	struct brkfs_sb_info *sbi = dir->sb->private_data;
	struct fs_super_block *sb = dir->sb;
	struct fs_inode *inode;
	u32 ino;
	int err;

	if (dentry->inode)
		return -EEXIST;
	err = brkfs_dir_lookup(dir, dentry->name.name, dentry->name.len, &ino,
			       NULL);
	if (err == 0)
		return -EEXIST;
	if (err != -ENOENT)
		return err;

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
			 u32 mask, unsigned int flags)
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

	/*
	 * Serialize all metadata updates with generic_file I/O and
	 * writeback.  Besides truncation, the mode/timestamp changes below
	 * are copied into the on-disk inode by brkfs_inode_write().
	 */
	sleeplock_acquire(&inode->rwsem);

	if (attr->valid & ATTR_MODE) {
		inode->mode = attr->mode;
		inode_touch_ctime(inode);
	}
	if (attr->valid & ATTR_SIZE) {
		/*
		 * Shrink: drop cache pages past the new EOF, zero the partial
		 * tail page, flush dirty data, then free on-disk blocks.
		 */
		if (inode->mapping) {
			err = truncate_inode_pages(inode->mapping, attr->size);
			if (!err)
				err = page_cache_flush(inode->mapping);
		}
		if (!err)
			err = brkfs_truncate_inode_blocks(inode, attr->size);
		if (err)
			goto out;
		inode_touch_mtime_ctime(inode);
	}
	err = brkfs_inode_write(sbi, inode);
out:
	sleeplock_release(&inode->rwsem);
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
	u32 bno;
	int err;
	u32 ino = inode->ino;
	u32 isize = sbi->s_sb.s_inode_size;
	struct brkfs_block bb;

	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		pr_warn("%s(): Invalid ino: %" PRIu32 "\n", __func__, ino);
		return -EINVAL;
	}

	bno = sbi->s_sb.s_inode_table + (ino - 1) / sbi->s_inodes_per_block;

	err = brkfs_get_block(sbi, bno, &bb);
	if (err)
		return err;

	cached_page_lock(bb.cp);

	u32 idx = (ino - 1) % sbi->s_inodes_per_block;
	disk_i = (struct brkfs_inode *)((u8 *)bb.data + idx * isize);

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
	u32 bno;
	u32 ino = inode->ino;
	int err;
	struct brkfs_inode *disk_i;
	struct brkfs_inode_info *info;
	u32 isize = sbi->s_sb.s_inode_size;
	struct brkfs_block bb;

	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		pr_warn("%s(): Invalid ino: %" PRIu32 "\n", __func__, ino);
		return -EINVAL;
	}

	bno = sbi->s_sb.s_inode_table + (ino - 1) / sbi->s_inodes_per_block;

	err = brkfs_get_block(sbi, bno, &bb);
	if (err)
		return err;

	cached_page_lock(bb.cp);

	u32 idx = (ino - 1) % sbi->s_inodes_per_block;
	disk_i = (struct brkfs_inode *)((u8 *)bb.data + idx * isize);

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

int brkfs_inode_alloc(struct brkfs_sb_info *sbi, u32 *ino)
{
	u32 bit = 0;
	int err;

	err = brkfs_bitmap_alloc(sbi, sbi->s_sb.s_inode_bitmap,
				 sbi->s_sb.s_inodes_count, &bit);
	if (err)
		return err;
	*ino = bit + 1;
	return 0;
}

int brkfs_inode_free(struct brkfs_sb_info *sbi, u32 ino)
{
	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		pr_warn("%s(): Invalid ino: %" PRIu32 "\n", __func__, ino);
		return -EINVAL;
	}
	ino -= 1;
	return brkfs_bitmap_free(sbi, sbi->s_sb.s_inode_bitmap,
				 sbi->s_sb.s_inodes_count, ino);
}

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, u32 ino, umode_t mode,
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

int brkfs_inode_getblk(struct fs_inode *inode, loff_t off, u32 *bno,
		       unsigned flags, struct brkfs_sb_info *sbi)
{
	struct brkfs_inode_info *inf = inode->private_data;
	u32 *blk_ptrs = inf->i_block;
	bool create = (flags & BRKFS_GETBLK_CREATE) != 0;
	int ret = 0;
	u32 bs = sbi->s_sb.s_blocksize;
	u32 ptrs_per_blk = sbi->s_sb.s_blocksize / sizeof(u32);
	struct brkfs_block bb;

	u32 blk_idx = off / sbi->s_sb.s_blocksize;
	if (blk_idx < BRKFS_DIRECT_BLOCKS) {
		if (blk_ptrs[blk_idx] == 0) {
			if (!create) { /* read of a hole or sparse region */
				*bno = 0;
				return 0;
			}
			u32 new_bno = 0;
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
		u32 idb = blk_ptrs[BRKFS_INDIRECT_BLOCK];
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

		u32 *idb_ptrs = bb.data;

		if (idb_ptrs[blk_idx] == 0) {
			if (!create) {
				*bno = 0;
				ret = 0;
				goto idb_out_unlock_and_put;
			}

			/* Unlock the inode block before allocating a new data block
			 * to avoid deadlocking with the page cache. */
			cached_page_unlock(bb.cp);
			u32 new_bno = 0;
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

	pr_warn("%s(): Double indirect block not implemented\n", __func__);

	return -ENOSPC;
}

int brkfs_truncate_inode_blocks(struct fs_inode *inode, loff_t new_size)
{
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	struct brkfs_inode_info *inf = inode->private_data;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t old_size = inode->size;
	u32 old_n;
	u32 new_n;
	u32 bi;

	if (!inf)
		return -EINVAL;
	if (new_size < 0)
		return -EINVAL;
	if (new_size >= old_size) {
		inode->size = new_size;
		return 0;
	}

	old_n = (u32)((old_size + (loff_t)bs - 1) / (loff_t)bs);
	new_n = (u32)((new_size + (loff_t)bs - 1) / (loff_t)bs);

	for (bi = new_n; bi < old_n && bi < BRKFS_DIRECT_BLOCKS; bi++) {
		if (inf->i_block[bi]) {
			brkfs_data_free(sbi, inf->i_block[bi]);
			inf->i_block[bi] = 0;
		}
	}

	/*
	 * Free the single-indirect range.  Double/triple indirect blocks are
	 * not implemented by the allocator, so the file cannot extend past
	 * this range.
	 */
	if (old_n > BRKFS_DIRECT_BLOCKS) {
		u32 idb = inf->i_block[BRKFS_INDIRECT_BLOCK];
		u32 ptrs_per_blk = bs / sizeof(u32);

		if (idb) {
			struct brkfs_block bb;
			u32 *ptrs;
			u32 start;
			u32 end;
			bool keep = false;

			if (brkfs_get_block(sbi, idb, &bb))
				return -EIO;

			cached_page_lock(bb.cp);
			ptrs = bb.data;

			start = new_n > BRKFS_DIRECT_BLOCKS ?
					new_n - BRKFS_DIRECT_BLOCKS :
					0;
			end = old_n - BRKFS_DIRECT_BLOCKS;
			if (end > ptrs_per_blk)
				end = ptrs_per_blk;

			for (u32 i = 0; i < ptrs_per_blk; i++) {
				if (i >= start && i < end) {
					if (ptrs[i]) {
						brkfs_data_free(sbi, ptrs[i]);
						ptrs[i] = 0;
					}
				} else if (ptrs[i]) {
					keep = true;
				}
			}

			cached_page_mark_dirty(bb.cp);
			cached_page_unlock(bb.cp);
			brkfs_put_block(&bb);

			if (!keep) {
				brkfs_data_free(sbi, idb);
				inf->i_block[BRKFS_INDIRECT_BLOCK] = 0;
			}
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
