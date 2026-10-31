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
#include <brk/task.h>
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

static bool brkfs_valid_data_bno(struct brkfs_sb_info *sbi, u32 bno)
{
	u64 first = sbi->s_sb.s_first_data_block;
	u64 end = first + sbi->s_sb.s_data_blocks_count;

	return bno >= first && bno < end;
}

static void brkfs_count_tree(struct brkfs_sb_info *sbi, u32 bno,
			     unsigned int level, u64 *blocks)
{
	u32 ptrs = sbi->s_sb.s_blocksize / sizeof(u32);
	u8 *buf;

	if (bno == 0 || !brkfs_valid_data_bno(sbi, bno))
		return;

	(*blocks)++;
	if (level == 0)
		return;

	buf = kmalloc(sbi->s_sb.s_blocksize);
	if (!buf)
		return;

	if (brkfs_block_read(sbi, bno, buf) == 0) {
		for (u32 i = 0; i < ptrs; i++) {
			u32 child = ((u32 *)buf)[i];

			if (child)
				brkfs_count_tree(sbi, child, level - 1, blocks);
		}
	}

	kfree(buf);
}

static u64 brkfs_inode_allocated_blocks(struct brkfs_sb_info *sbi,
					struct fs_inode *inode)
{
	struct brkfs_inode_info *info = inode->private_data;
	u64 blocks = 0;

	if (!info)
		return 0;

	for (unsigned int i = 0; i < BRKFS_DIRECT_BLOCKS; i++) {
		if (brkfs_valid_data_bno(sbi, info->i_block[i]) &&
		    info->i_block[i] != 0)
			blocks++;
	}

	for (unsigned int i = 0; i < 3; i++) {
		u32 root = info->i_block[BRKFS_INDIRECT_BLOCK + i];

		if (root != 0)
			brkfs_count_tree(sbi, root, i + 1, &blocks);
	}

	return blocks;
}

/*
 * A newly allocated inode has not been linked into a directory yet.  Mark it
 * deleted so the normal eviction path frees its data blocks and inode slot
 * instead of leaving an orphan behind.
 */
static void brkfs_discard_new_inode(struct fs_inode *inode)
{
	inode->nlink = 0;
	fs_inode_put(inode);
}

static void brkfs_rollback_dir_entry(struct fs_inode *dir,
				     struct fs_dentry *dentry,
				     struct fs_inode *inode)
{
	(void)brkfs_dir_remove(dir, dentry->name.name, dentry->name.len);
	brkfs_discard_new_inode(inode);
}

static void brkfs_orphan_add(struct brkfs_sb_info *sbi, struct fs_inode *inode)
{
	struct brkfs_inode_info *info = inode->private_data;

	if (!info)
		return;

	sleeplock_acquire(&sbi->s_orphan_lock);
	spin_lock(&sbi->s_super_lock);
	info->i_next_orphan = sbi->s_sb.s_orphan;
	sbi->s_sb.s_orphan = (u32)inode->ino;
	sbi->s_super_dirty = true;
	spin_unlock(&sbi->s_super_lock);
	sleeplock_release(&sbi->s_orphan_lock);
}

void brkfs_orphan_remove(struct brkfs_sb_info *sbi, u32 ino)
{
	u32 prev = 0;
	u32 cur;

	sleeplock_acquire(&sbi->s_orphan_lock);

	cur = sbi->s_sb.s_orphan;
	u32 guard = sbi->s_sb.s_inodes_count;

	while (cur && guard--) {
		struct brkfs_inode_info info;
		struct fs_inode inode = {
			.ino = cur,
			.private_data = &info,
		};
		u32 next;

		if (brkfs_inode_read(sbi, &inode))
			break;
		next = info.i_next_orphan;

		if (cur == ino) {
			if (prev) {
				struct brkfs_inode_info pinfo;
				struct fs_inode prev_inode = {
					.ino = prev,
					.private_data = &pinfo,
				};

				if (!brkfs_inode_read(sbi, &prev_inode)) {
					pinfo.i_next_orphan = next;
					(void)brkfs_inode_write(sbi,
								&prev_inode);
				}
			}
			spin_lock(&sbi->s_super_lock);
			if (!prev)
				sbi->s_sb.s_orphan = next;
			sbi->s_super_dirty = true;
			spin_unlock(&sbi->s_super_lock);
			break;
		}

		prev = cur;
		cur = next;
	}

	sleeplock_release(&sbi->s_orphan_lock);
}

void brkfs_orphan_cleanup(struct brkfs_sb_info *sbi)
{
	struct fs_super_block fake_sb = {
		.private_data = sbi,
		.block_size = sbi->s_sb.s_blocksize,
	};
	u32 ino;

	ino = sbi->s_sb.s_orphan;
	u32 guard = sbi->s_sb.s_inodes_count;

	while (ino && guard--) {
		struct brkfs_inode_info info;
		struct fs_inode inode = {
			.ino = ino,
			.sb = &fake_sb,
			.private_data = &info,
		};
		u32 next;

		if (brkfs_inode_read(sbi, &inode)) {
			sbi->s_sb.s_orphan = ino;
			sbi->s_super_dirty = true;
			return;
		}

		next = info.i_next_orphan;
		(void)brkfs_truncate_inode_blocks(&inode, 0);
		inode.size = 0;
		inode.nlink = 0;
		info.i_next_orphan = 0;
		(void)brkfs_inode_write(sbi, &inode);
		(void)brkfs_inode_free(sbi, ino);
		ino = next;
	}

	if (ino) {
		spin_lock(&sbi->s_super_lock);
		sbi->s_sb.s_orphan = ino;
		sbi->s_super_dirty = true;
		spin_unlock(&sbi->s_super_lock);
		return;
	}

	spin_lock(&sbi->s_super_lock);
	sbi->s_sb.s_orphan = 0;
	sbi->s_super_dirty = true;
	spin_unlock(&sbi->s_super_lock);
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
	err = brkfs_disk_inode_init(sbi, ino, S_IFREG | mode, 1, 0,
				    current_task()->fsuid,
				    current_task()->fsgid);
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
		brkfs_discard_new_inode(inode);
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
	if (inode->nlink == 0)
		brkfs_orphan_add(sbi, inode);

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
	err = brkfs_disk_inode_init(sbi, ino, S_IFLNK | 0777, 1, 0,
				    current_task()->fsuid,
				    current_task()->fsgid);
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
		err = err ? err : -EIO;
		brkfs_discard_new_inode(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->name.name, dentry->name.len,
			    S_IFLNK | 0777);
	if (err) {
		brkfs_discard_new_inode(inode);
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
	err = brkfs_disk_inode_init(sbi, ino, (umode_t)(S_IFDIR | mode), 2, 0,
				    current_task()->fsuid,
				    current_task()->fsgid);
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
		brkfs_discard_new_inode(inode);
		goto out;
	}
	err = brkfs_inode_write(sbi, inode);
	if (err) {
		brkfs_discard_new_inode(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->name.name, dentry->name.len,
			    (umode_t)(S_IFDIR | mode));
	if (err) {
		brkfs_discard_new_inode(inode);
		goto out;
	}

	dir->nlink++;
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err) {
		dir->nlink--;
		brkfs_rollback_dir_entry(dir, dentry, inode);
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
	brkfs_orphan_add(sbi, inode);
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
			brkfs_orphan_add(sbi, target);
			inode_touch_ctime(target);
			err = brkfs_inode_write(sbi, target);
			if (!err) {
				new_dir->nlink--;
				inode_touch_mtime_ctime(new_dir);
			}
		} else {
			target->nlink--;
			if (target->nlink == 0)
				brkfs_orphan_add(sbi, target);
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
	err = brkfs_disk_inode_init(sbi, ino, mode, 1, dev,
				    current_task()->fsuid,
				    current_task()->fsgid);
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
		brkfs_discard_new_inode(inode);
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
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	u64 blocks;

	(void)mask;
	(void)flags;

	sleeplock_acquire(&inode->rwsem);

	memset(stat, 0, sizeof(*stat));
	stat->st_ino = inode->ino;
	stat->st_mode = inode->mode;
	stat->st_nlink = inode->nlink;
	stat->st_uid = inode->uid;
	stat->st_gid = inode->gid;
	stat->st_rdev = inode->rdev;
	stat->st_size = inode->size;
	stat->st_blksize = (int)inode->sb->block_size;
	blocks = brkfs_inode_allocated_blocks(sbi, inode);
	if (blocks == 0 && inode->size > 0)
		blocks = div_ceil(inode->size, inode->sb->block_size);
	stat->st_blocks = (long)div_ceil(blocks * inode->sb->block_size, 512);
	inode_times_to_stat(inode, stat);

	sleeplock_release(&inode->rwsem);
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
		inode->mode = (inode->mode & S_IFMT) | (attr->mode & 07777);
		inode_touch_ctime(inode);
	}
	if (attr->valid & (ATTR_UID | ATTR_GID)) {
		if (attr->valid & ATTR_UID)
			inode->uid = attr->uid;
		if (attr->valid & ATTR_GID)
			inode->gid = attr->gid;
		inode->mode &= ~(S_ISUID | S_ISGID);
		inode_touch_ctime(inode);
	}
	if (attr->valid & (ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)) {
		inode_times_set(inode,
				(attr->valid & ATTR_ATIME) ? &attr->atime : NULL,
				(attr->valid & ATTR_MTIME) ? &attr->mtime : NULL);
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

	if (disk_i->i_ino != ino) {
		cached_page_unlock(bb.cp);
		brkfs_put_block(&bb);
		pr_warn("%s(): inode %" PRIu32 " has on-disk i_ino %" PRIu32
			"\n", __func__, ino, disk_i->i_ino);
		return -EIO;
	}

	if (!brkfs_inode_checksum_ok(disk_i)) {
		cached_page_unlock(bb.cp);
		brkfs_put_block(&bb);
		pr_warn("%s(): inode %" PRIu32 " has a bad checksum\n",
			__func__, ino);
		return -EIO;
	}

	info = inode->private_data;

	inode->mode = disk_i->i_mode;
	inode->rdev = disk_i->i_rdev;
	inode->nlink = disk_i->i_nlink;
	inode->size = disk_i->i_size;
	memcpy(info->i_block, disk_i->i_block, sizeof(disk_i->i_block));
	info->i_next_orphan = disk_i->i_next_orphan;
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

	disk_i->i_ino = ino;
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
	disk_i->i_next_orphan = info->i_next_orphan;
	brkfs_inode_set_checksum(disk_i);

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
	spin_lock(&sbi->s_super_lock);
	if (sbi->s_sb.s_free_inodes > 0)
		sbi->s_sb.s_free_inodes--;
	sbi->s_super_dirty = true;
	spin_unlock(&sbi->s_super_lock);
	*ino = bit + 1;
	return 0;
}

int brkfs_inode_free(struct brkfs_sb_info *sbi, u32 ino)
{
	int err;

	if (ino < 1 || ino > sbi->s_sb.s_inodes_count) {
		pr_warn("%s(): Invalid ino: %" PRIu32 "\n", __func__, ino);
		return -EINVAL;
	}
	ino -= 1;
	err = brkfs_bitmap_free(sbi, sbi->s_sb.s_inode_bitmap,
				sbi->s_sb.s_inodes_count, ino);
	if (!err) {
		spin_lock(&sbi->s_super_lock);
		sbi->s_sb.s_free_inodes++;
		sbi->s_super_dirty = true;
		spin_unlock(&sbi->s_super_lock);
	}
	return err;
}

int brkfs_disk_inode_init(struct brkfs_sb_info *sbi, u32 ino, umode_t mode,
			  unsigned int nlink, dev_t rdev, kuid_t uid,
			  kgid_t gid)
{
	struct fs_inode stub = { 0 };
	struct brkfs_inode_info info;

	memset(&info, 0, sizeof(info));
	stub.ino = ino;
	stub.mode = mode;
	stub.nlink = nlink;
	stub.size = 0;
	stub.rdev = rdev;
	stub.uid = uid;
	stub.gid = gid;
	stub.private_data = &info;
	inode_times_set_all_now(&stub);
	return brkfs_inode_write(sbi, &stub);
}

static int brkfs_alloc_zeroed_block(struct brkfs_sb_info *sbi, u32 *out)
{
	u32 bno;
	u8 *buf;
	int err;

	err = brkfs_data_alloc(sbi, &bno);
	if (err)
		return err;

	buf = kmalloc(sbi->s_sb.s_blocksize);
	if (!buf) {
		brkfs_data_free(sbi, bno);
		return -ENOMEM;
	}

	memset(buf, 0, sbi->s_sb.s_blocksize);
	err = brkfs_block_write(sbi, bno, buf);
	kfree(buf);
	if (err) {
		brkfs_data_free(sbi, bno);
		return err;
	}

	*out = bno;
	return 0;
}

static int brkfs_read_ptr(struct brkfs_sb_info *sbi, u32 bno, u32 index,
			  u32 *out)
{
	u8 *buf = kmalloc(sbi->s_sb.s_blocksize);
	int err;

	if (!buf)
		return -ENOMEM;
	err = brkfs_block_read(sbi, bno, buf);
	if (!err)
		*out = ((u32 *)buf)[index];
	kfree(buf);
	return err;
}

static int brkfs_set_ptr_if_zero(struct brkfs_sb_info *sbi, u32 bno,
				 u32 index, u32 new_bno, u32 *out)
{
	u8 *buf = kmalloc(sbi->s_sb.s_blocksize);
	int err;

	if (!buf)
		return -ENOMEM;

	err = brkfs_block_read(sbi, bno, buf);
	if (err) {
		kfree(buf);
		return err;
	}

	u32 *ptrs = (u32 *)buf;

	if (ptrs[index] == 0) {
		ptrs[index] = new_bno;
		err = brkfs_block_write(sbi, bno, buf);
		*out = new_bno;
	} else {
		*out = ptrs[index];
		brkfs_data_free(sbi, new_bno);
	}
	kfree(buf);
	return err;
}

static int brkfs_get_leaf(struct brkfs_sb_info *sbi, u32 parent, u32 index,
			  bool create, u32 *out)
{
	u32 bno;
	int err;

	err = brkfs_read_ptr(sbi, parent, index, &bno);
	if (err)
		return err;

	if (bno != 0) {
		if (!brkfs_valid_data_bno(sbi, bno))
			return -EIO;
		*out = bno;
		return 0;
	}

	if (!create) {
		*out = 0;
		return 0;
	}

	u32 new_bno = 0;

	err = brkfs_data_alloc(sbi, &new_bno);
	if (err)
		return err;

	err = brkfs_set_ptr_if_zero(sbi, parent, index, new_bno, &bno);
	if (err)
		return err;
	if (!brkfs_valid_data_bno(sbi, bno))
		return -EIO;

	*out = bno;
	return 0;
}

static int brkfs_indirect_lookup(struct brkfs_sb_info *sbi, u32 *root,
				 unsigned int level, u64 idx, bool create,
				 u32 *out)
{
	u32 ptrs = sbi->s_sb.s_blocksize / sizeof(u32);
	u64 shift = 1;
	u32 top;
	u64 next;
	u32 child;
	int err;

	if (*root == 0) {
		if (!create) {
			*out = 0;
			return 0;
		}
		err = brkfs_alloc_zeroed_block(sbi, root);
		if (err)
			return err;
	}

	if (!brkfs_valid_data_bno(sbi, *root))
		return -EIO;

	if (level == 1)
		return brkfs_get_leaf(sbi, *root, (u32)idx, create, out);

	for (unsigned int i = 0; i < level - 1; i++)
		shift *= ptrs;

	top = (u32)(idx / shift);
	next = idx % shift;
	if (top >= ptrs)
		return -ENOSPC;

	err = brkfs_read_ptr(sbi, *root, top, &child);
	if (err)
		return err;

	if (child == 0) {
		if (!create) {
			*out = 0;
			return 0;
		}

		u32 new_bno = 0;

		err = brkfs_alloc_zeroed_block(sbi, &new_bno);
		if (err)
			return err;
		err = brkfs_set_ptr_if_zero(sbi, *root, top, new_bno, &child);
		if (err)
			return err;
	}

	if (!brkfs_valid_data_bno(sbi, child))
		return -EIO;

	return brkfs_indirect_lookup(sbi, &child, level - 1, next, create,
				     out);
}

int brkfs_inode_getblk(struct fs_inode *inode, loff_t off, u32 *bno,
		       unsigned flags, struct brkfs_sb_info *sbi)
{
	struct brkfs_inode_info *inf = inode->private_data;
	u32 *blk_ptrs = inf->i_block;
	bool create = (flags & BRKFS_GETBLK_CREATE) != 0;
	u32 bs = sbi->s_sb.s_blocksize;
	u32 ptrs = bs / sizeof(u32);
	u64 blk_idx = (u64)off / bs;

	if (blk_idx < BRKFS_DIRECT_BLOCKS) {
		u32 idx = (u32)blk_idx;

		if (blk_ptrs[idx] == 0) {
			if (!create) { /* read of a hole or sparse region */
				*bno = 0;
				return 0;
			}

			u32 new_bno = 0;
			int err = brkfs_data_alloc(sbi, &new_bno);

			if (err)
				return err;
			blk_ptrs[idx] = new_bno;
			*bno = new_bno;
			return 0;
		}

		if (!brkfs_valid_data_bno(sbi, blk_ptrs[idx]))
			return -EIO;
		*bno = blk_ptrs[idx];
		return 0;
	}

	u64 adj = blk_idx - BRKFS_DIRECT_BLOCKS;
	u64 span1 = ptrs;
	u64 span2 = (u64)ptrs * ptrs;
	u64 span3 = span2 * ptrs;
	u64 capacity = span1 + span2 + span3;
	unsigned int level;
	u64 idx;

	if (adj >= capacity)
		return -ENOSPC;

	if (adj < span1) {
		level = 1;
		idx = adj;
	} else if (adj < span1 + span2) {
		level = 2;
		idx = adj - span1;
	} else {
		level = 3;
		idx = adj - span1 - span2;
	}

	if (create) {
		u32 features = 0;

		if (S_ISDIR(inode->mode))
			features |= BRKFS_FEATURE_DIR_INDIRECT;
		if (level >= 2)
			features |= BRKFS_FEATURE_DOUBLE_INDIRECT;
		if (level == 3)
			features |= BRKFS_FEATURE_TRIPLE_INDIRECT;

		if (features) {
			int err = brkfs_super_set_features(sbi, features);

			if (err)
				return err;
		}
	}

	return brkfs_indirect_lookup(sbi,
				     &blk_ptrs[BRKFS_INDIRECT_BLOCK + level - 1],
				     level, idx, create, bno);
}

static u64 brkfs_indirect_span(u32 ptrs, unsigned int level)
{
	u64 span = 1;

	for (unsigned int i = 0; i < level; i++)
		span *= ptrs;
	return span;
}

static void brkfs_free_tree(struct brkfs_sb_info *sbi, u32 bno,
			    unsigned int level)
{
	u32 ptrs = sbi->s_sb.s_blocksize / sizeof(u32);
	u8 *buf;

	if (bno == 0 || !brkfs_valid_data_bno(sbi, bno))
		return;

	if (level > 0) {
		buf = kmalloc(sbi->s_sb.s_blocksize);
		if (buf && brkfs_block_read(sbi, bno, buf) == 0) {
			u32 *children = (u32 *)buf;

			for (u32 i = 0; i < ptrs; i++)
				if (children[i])
					brkfs_free_tree(sbi, children[i],
							level - 1);
		}
		kfree(buf);
	}

	brkfs_data_free(sbi, bno);
}

/*
 * Free blocks in an indirect subtree whose logical block numbers (relative to
 * the first indirect block) are >= @free_from.  The subtree covers
 * [@base, @end).  *@keep is set when any part of the subtree must stay.
 */
static void brkfs_free_indirect_range(struct brkfs_sb_info *sbi, u32 *root,
				      unsigned int level, u64 base, u64 end,
				      u64 free_from, bool *keep)
{
	u32 ptrs = sbi->s_sb.s_blocksize / sizeof(u32);
	u64 child_span = brkfs_indirect_span(ptrs, level - 1);
	u8 *buf;
	bool any_keep = false;
	bool changed = false;

	if (*root == 0) {
		*keep = false;
		return;
	}
	if (!brkfs_valid_data_bno(sbi, *root)) {
		*root = 0;
		*keep = false;
		return;
	}

	if (base >= free_from) {
		brkfs_free_tree(sbi, *root, level);
		*root = 0;
		*keep = false;
		return;
	}
	if (end <= free_from) {
		*keep = true;
		return;
	}

	buf = kmalloc(sbi->s_sb.s_blocksize);
	if (!buf) {
		*keep = true;
		return;
	}
	if (brkfs_block_read(sbi, *root, buf) != 0) {
		kfree(buf);
		*keep = true;
		return;
	}

	u32 *children = (u32 *)buf;

	for (u32 i = 0; i < ptrs; i++) {
		u64 child_base = base + (u64)i * child_span;
		u64 child_end = child_base + child_span;

		if (children[i] == 0)
			continue;
		if (child_base >= free_from) {
			brkfs_free_tree(sbi, children[i], level - 1);
			children[i] = 0;
			changed = true;
		} else if (child_end <= free_from) {
			any_keep = true;
		} else {
			bool child_keep = false;

			brkfs_free_indirect_range(sbi, &children[i], level - 1,
						  child_base, child_end,
						  free_from, &child_keep);
			changed = true;
			if (child_keep)
				any_keep = true;
		}
	}

	if (changed)
		(void)brkfs_block_write(sbi, *root, buf);
	kfree(buf);

	if (!any_keep) {
		brkfs_data_free(sbi, *root);
		*root = 0;
	}
	*keep = any_keep;
}

int brkfs_truncate_inode_blocks(struct fs_inode *inode, loff_t new_size)
{
	struct brkfs_sb_info *sbi = inode->sb->private_data;
	struct brkfs_inode_info *inf = inode->private_data;
	u32 bs = sbi->s_sb.s_blocksize;
	loff_t old_size = inode->size;
	u64 new_n;
	u32 ptrs;

	if (!inf)
		return -EINVAL;
	if (new_size < 0)
		return -EINVAL;
	if (new_size > old_size) {
		inode->size = new_size;
		return 0;
	}

	new_n = ((u64)new_size + bs - 1) / bs;

	for (u32 bi = 0; bi < BRKFS_DIRECT_BLOCKS; bi++) {
		u32 bno;

		if ((u64)bi < new_n)
			continue;
		bno = inf->i_block[bi];
		if (bno) {
			if (brkfs_valid_data_bno(sbi, bno))
				brkfs_data_free(sbi, bno);
			inf->i_block[bi] = 0;
		}
	}

	u64 free_from = new_n > BRKFS_DIRECT_BLOCKS ?
				new_n - BRKFS_DIRECT_BLOCKS :
				0;
	u64 base = 0;

	ptrs = bs / sizeof(u32);
	for (unsigned int level = 1; level <= 3; level++) {
		u64 span = brkfs_indirect_span(ptrs, level);
		u64 end = base + span;
		u32 *root = &inf->i_block[BRKFS_INDIRECT_BLOCK + level - 1];

		if (*root != 0) {
			bool keep = false;

			brkfs_free_indirect_range(sbi, root, level, base, end,
						  free_from, &keep);
		}
		base = end;
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
