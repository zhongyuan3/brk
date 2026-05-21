#include "brkfs.h"
#include <brk/dcache.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/lock.h>
#include <brk/stat.h>
#include <brk/string.h>

#define ATTR_MODE (1u << 0)
#define ATTR_SIZE (1u << 3)

static void brkfs_inode_attach_ops(struct fs_inode *inode)
{
	inode->i_op = &brkfs_iops;
	if (S_ISDIR(inode->i_mode))
		inode->i_fop = &brkfs_dir_fops;
	else if (S_ISREG(inode->i_mode))
		inode->i_fop = &brkfs_file_fops;
	else if (S_ISLNK(inode->i_mode))
		inode->i_fop = &brkfs_file_fops;
	else if (S_ISCHR(inode->i_mode))
		inode->i_fop = &chrdev_fops;
	else if (S_ISBLK(inode->i_mode))
		inode->i_fop = &blkdev_fops;
	else
		inode->i_fop = &brkfs_file_fops;
}

static int brkfs_init_loaded_inode(struct brkfs_sb_info *sbi,
				   struct fs_inode *inode)
{
	int err = brkfs_inode_read(sbi, inode);

	if (err)
		return err;
	brkfs_inode_attach_ops(inode);
	/*
	 * Regular files are read/written through the page cache. Other inode
	 * types use bespoke paths (directory entries / symlink targets are
	 * handled directly via brkfs_block_read|write).
	 */
	if (S_ISREG(inode->i_mode)) {
		err = inode_attach_pagecache(inode, &brkfs_aops);
		if (err)
			return err;
	}
	inode_unlock_new(inode);
	return 0;
}

static struct path_component *brkfs_lookup(struct fs_inode *dir,
					 struct path_component *dentry,
					 unsigned int flags)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_state *sb = dir->i_sb;
	struct fs_inode *inode;
	u32 ino;
	u8 type;
	int err;

	(void)flags;

	if (!S_ISDIR(dir->i_mode))
		return ERR_PTR(-ENOTDIR);

	err = brkfs_dir_lookup(dir, dentry->d_name.name, dentry->d_name.len,
			       &ino, &type);
	if (err == -ENOENT)
		return NULL;
	if (err)
		return ERR_PTR(err);

	inode = inode_get_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	spinlock_acquire(&inode->i_lock);
	bool is_new = (inode->i_state & I_NEW) != 0;
	spinlock_release(&inode->i_lock);
	if (is_new) {
		err = brkfs_init_loaded_inode(sbi, inode);
		if (err) {
			inode_put(inode);
			return ERR_PTR(err);
		}
	}

	return dentry_splice_alias(inode, dentry);
}

static int brkfs_create(struct fs_inode *dir, struct path_component *dentry,
			umode_t mode, bool excl)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_state *sb = dir->i_sb;
	struct fs_inode *inode = NULL;
	u32 ino = 0;
	int err;

	(void)excl;

	err = brkfs_dir_lookup(dir, dentry->d_name.name, dentry->d_name.len,
			       &ino, NULL);
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
	err = brkfs_disk_inode_init(sbi, ino, (umode_t)(S_IFREG | mode), 1, 0);
	if (err)
		goto undo_alloc;

	inode = inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		inode_put(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->d_name.name, dentry->d_name.len,
			    (umode_t)(S_IFREG | mode));
	if (err) {
		inode_put(inode);
		goto out;
	}

	dentry_instantiate(dentry, inode);
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

static int brkfs_link(struct path_component *old_dentry, struct fs_inode *dir,
		      struct path_component *new_dentry)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_inode *old_inode = old_dentry->d_inode;
	int err;

	err = brkfs_dir_add(dir, (u32)old_inode->i_ino, new_dentry->d_name.name,
			    new_dentry->d_name.len, old_inode->i_mode);
	if (err)
		goto out;
	inode_dup(old_inode);
	old_inode->i_nlink++;
	inode_touch_ctime(old_inode);
	err = brkfs_inode_write(sbi, old_inode);
	if (err) {
		old_inode->i_nlink--;
		inode_put(old_inode);
		goto out;
	}
	dentry_instantiate(new_dentry, old_inode);
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
out:
	return err;
}

static int brkfs_unlink(struct fs_inode *dir, struct path_component *dentry)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_inode *inode = dentry->d_inode;
	int err;

	err = brkfs_dir_remove(dir, dentry->d_name.name, dentry->d_name.len);
	if (err)
		return err;
	inode->i_nlink--;
	inode_touch_ctime(inode);
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err)
		return err;
	return brkfs_inode_write(sbi, inode);
	/*
	 * Do NOT inode_put(inode) here: the inode reference belongs to the
	 * dentry. The eventual dentry_put() will drop it and trigger
	 * eviction (which now also runs the page cache teardown).
	 */
}

static int brkfs_symlink(struct fs_inode *dir, struct path_component *dentry,
			 const char *symname)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_state *sb = dir->i_sb;
	struct fs_inode *inode = NULL;
	u32 ino = 0;
	usize_t len = strlen(symname);
	loff_t pos = 0;
	usize_t w = 0;
	int err;

	err = brkfs_inode_alloc(sbi, &ino);
	if (err)
		goto out;
	err = brkfs_disk_inode_init(sbi, ino, S_IFLNK | 0777, 1, 0);
	if (err)
		goto undo_alloc;

	inode = inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		inode_put(inode);
		goto out;
	}

	err = brkfs_file_write_at(inode, &pos, symname, len, &w);
	if (err || w != len) {
		inode_put(inode);
		err = err ? err : -EIO;
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->d_name.name, dentry->d_name.len,
			    S_IFLNK | 0777);
	if (err) {
		inode_put(inode);
		goto out;
	}

	dentry_instantiate(dentry, inode);
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

static int brkfs_readlink(struct path_component *dentry, char *buf, int bufsiz)
{
	struct fs_inode *inode = dentry->d_inode;
	loff_t pos = 0;
	usize_t rd = 0;
	int err;

	if (!S_ISLNK(inode->i_mode))
		return -EINVAL;
	if (bufsiz <= 0)
		return -EINVAL;
	err = brkfs_file_read_at(inode, &pos, buf, (usize_t)(bufsiz - 1), &rd);
	if (err)
		return err;
	buf[rd] = '\0';
	return (int)rd;
}

static int brkfs_mkdir(struct fs_inode *dir, struct path_component *dentry,
		       umode_t mode)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_state *sb = dir->i_sb;
	struct fs_inode *inode = NULL;
	u32 ino = 0;
	int err;

	err = brkfs_inode_alloc(sbi, &ino);
	if (err)
		goto out;
	err = brkfs_disk_inode_init(sbi, ino, (umode_t)(S_IFDIR | mode), 2, 0);
	if (err)
		goto undo_alloc;

	inode = inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		inode_put(inode);
		goto out;
	}

	err = brkfs_new_dir_body(inode, (u32)dir->i_ino);
	if (err) {
		inode_put(inode);
		goto out;
	}
	err = brkfs_inode_write(sbi, inode);
	if (err) {
		inode_put(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->d_name.name, dentry->d_name.len,
			    (umode_t)(S_IFDIR | mode));
	if (err) {
		inode_put(inode);
		goto out;
	}

	dir->i_nlink++;
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err) {
		dir->i_nlink--;
		inode_put(inode);
		goto out;
	}

	dentry_instantiate(dentry, inode);
	err = 0;
	goto out;

undo_alloc:
	brkfs_inode_free(sbi, ino);
out:
	return err;
}

static int brkfs_rmdir(struct fs_inode *dir, struct path_component *dentry)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_inode *inode = dentry->d_inode;
	int err;

	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;
	if (inode->i_nlink > 2)
		return -ENOTEMPTY;

	err = brkfs_dir_remove(dir, dentry->d_name.name, dentry->d_name.len);
	if (err)
		return err;
	dir->i_nlink--;
	inode_touch_mtime_ctime(dir);
	err = brkfs_inode_write(sbi, dir);
	if (err)
		return err;
	inode->i_nlink = 0;
	inode_touch_ctime(inode);
	return brkfs_inode_write(sbi, inode);
	/*
	 * Do NOT inode_put(inode) here. The inode reference belongs to the
	 * dentry; dentry_put() will trigger the final eviction.
	 */
}

static int brkfs_rename(struct fs_inode *old_dir,
			struct path_component *old_dentry,
			struct fs_inode *new_dir,
			struct path_component *new_dentry, unsigned int flags)
{
	(void)old_dir;
	(void)old_dentry;
	(void)new_dir;
	(void)new_dentry;
	(void)flags;
	return -EOPNOTSUPP;
}

static int brkfs_mknod(struct fs_inode *dir, struct path_component *dentry,
		       umode_t mode, dev_t dev)
{
	struct brkfs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct fs_state *sb = dir->i_sb;
	struct fs_inode *inode;
	u32 ino;
	int err;

	err = brkfs_inode_alloc(sbi, &ino);
	if (err)
		goto out;
	err = brkfs_disk_inode_init(sbi, ino, mode, 1, dev);
	if (err)
		goto undo_alloc;

	inode = inode_get_locked(sb, ino);
	if (!inode) {
		err = -ENOMEM;
		goto undo_alloc;
	}
	err = brkfs_init_loaded_inode(sbi, inode);
	if (err) {
		inode_put(inode);
		goto out;
	}

	err = brkfs_dir_add(dir, ino, dentry->d_name.name, dentry->d_name.len,
			    mode);
	if (err) {
		inode_put(inode);
		goto out;
	}

	dentry_instantiate(dentry, inode);
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

static int brkfs_getattr(const struct file_anchor *path, struct stat *stat,
			 u32 mask, unsigned int flags)
{
	struct fs_inode *inode = path->dentry->d_inode;

	(void)mask;
	(void)flags;

	memset(stat, 0, sizeof(*stat));
	stat->st_ino = inode->i_ino;
	stat->st_mode = inode->i_mode;
	stat->st_nlink = inode->i_nlink;
	stat->st_rdev = inode->i_rdev;
	stat->st_size = inode->i_size;
	stat->st_blksize = (int)inode->i_sb->s_blocksize;
	stat->st_blocks = div_ceil(inode->i_size, inode->i_sb->s_blocksize);
	inode_times_to_stat(inode, stat);
	return 0;
}

static int brkfs_setattr(struct path_component *dentry, struct fs_inode_attr *attr)
{
	struct fs_inode *inode = dentry->d_inode;
	struct brkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	int err = 0;

	if (attr->ia_valid & ATTR_MODE) {
		inode->i_mode = attr->ia_mode;
		inode_touch_ctime(inode);
	}
	if (attr->ia_valid & ATTR_SIZE) {
		/* Drop cached pages that are beyond the new size before freeing
		 * the on-disk blocks, otherwise stale data would be visible to
		 * concurrent readers. */
		truncate_inode_pages(inode->i_mapping, attr->ia_size);
		err = brkfs_truncate_inode_blocks(inode, attr->ia_size);
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
