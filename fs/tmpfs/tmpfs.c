#include "internal.h"
#include <aosd/assert.h>
#include <aosd/dcache.h>
#include <aosd/dev.h>
#include <aosd/errno.h>
#include <aosd/fs.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/mount.h>
#include <aosd/path.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/types.h>
#include <uapi/aosd/dirent.h>
#include <uapi/aosd/fcntl.h>
#include <uapi/aosd/stat.h>

static list_head_define(tmpfs_roots);
static spinlock_define(tmpfs_lock);

static struct tmpfs_inode *tmpfs_inode_alloc(dev_t dev, uint32_t flags,
					     mode_t mode)
{
	struct tmpfs_inode *inode;
	static uint32_t node_id = 0;

	inode = kmalloc(sizeof(*inode));
	if (!inode)
		return NULL;

	inode->i_dev = dev;
	inode->i_flags = flags;
	inode->i_links = 1;
	inode->i_inum = node_id++;
	inode->i_mode = mode;
	spinlock_init(&inode->i_lock, "tmpfs_inode");
	return inode;
}

static void tmpfs_inode_free(struct tmpfs_inode *ip)
{
	kfree(ip);
}

static struct tmpfs_node *tmpfs_node_alloc(const char *name, size_t len)
{
	struct tmpfs_node *node;
	char *node_name;

	node_name = kmalloc(len + 1);
	if (!node_name)
		return NULL;
	memcpy(node_name, name, len);
	node_name[len] = '\0';

	node = kzalloc(sizeof(*node));
	if (!node) {
		kfree(node_name);
		return NULL;
	}

	list_init_head(&node->n_subnodes);
	list_init_head(&node->n_sub);
	list_init_head(&node->n_root);

	node->n_name = node_name;
	node->n_inode = NULL;
	return node;
}

static void tmpfs_node_free(struct tmpfs_node *node)
{
	kfree(node->n_name);
	kfree(node);
}

static void tmpfs_destroy_tree(struct tmpfs_node *node)
{
	struct tmpfs_node *curr, *next;

	if ((node->n_inode->i_mode & S_IFDIR) &&
	    !list_empty(&node->n_subnodes)) {
		list_for_each_entry_safe(curr, next, &node->n_subnodes, n_sub) {
			tmpfs_destroy_tree(curr);
			list_del(&curr->n_sub);
		}
	}

	if (node->n_inode->i_links > 0)
		node->n_inode->i_links -= 1;
	if (node->n_inode->i_links == 0)
		tmpfs_inode_free(node->n_inode);

	tmpfs_node_free(node);
}

static void tmpfs_read_inode(struct inode *inode, struct tmpfs_node *node)
{
	assert(sleeplock_holding(&inode->i_lock));
	spinlock_acquire(&node->n_inode->i_lock);
	inode->i_num = node->n_inode->i_inum;
	inode->i_dev = node->n_inode->i_dev;
	inode->i_flags = node->n_inode->i_flags;
	inode->i_mode = node->n_inode->i_mode;
	inode->i_links = node->n_inode->i_links;
	inode->i_size = sizeof(struct tmpfs_node);
	inode->i_private = node;
	spinlock_release(&node->n_inode->i_lock);
}

static struct tmpfs_node *tmpfs_dir_find_entry(struct tmpfs_node *dir_node,
					       const char *name)
{
	struct tmpfs_node *curr;

	spinlock_acquire(&tmpfs_lock);

	if (list_empty(&dir_node->n_subnodes)) {
		spinlock_release(&tmpfs_lock);
		return NULL;
	}

	list_for_each_entry(curr, &dir_node->n_subnodes, n_sub) {
		if (strcmp(name, curr->n_name) == 0) {
			spinlock_release(&tmpfs_lock);
			return curr;
		}
	}

	spinlock_release(&tmpfs_lock);
	return NULL;
}

static void tmpfs_dir_add_entry(struct tmpfs_node *dir_node,
				struct tmpfs_node *sub_node)
{
	spinlock_acquire(&tmpfs_lock);
	list_add(&sub_node->n_sub, &dir_node->n_subnodes);
	spinlock_release(&tmpfs_lock);
}

static void tmpfs_dir_del_entry(struct tmpfs_node *sub_node)
{
	spinlock_acquire(&tmpfs_lock);
	list_del(&sub_node->n_sub);
	spinlock_release(&tmpfs_lock);
}

static void tmpfs_add_root(struct tmpfs_node *root)
{
	spinlock_acquire(&tmpfs_lock);
	list_add(&root->n_root, &tmpfs_roots);
	spinlock_release(&tmpfs_lock);
}

static mode_t tmpfs_inode_mode(struct tmpfs_inode *inode)
{
	mode_t mode;
	spinlock_acquire(&inode->i_lock);
	mode = inode->i_mode;
	spinlock_release(&inode->i_lock);
	return mode;
}

static uint32_t tmpfs_inode_links(struct tmpfs_inode *inode)
{
	uint32_t links;
	spinlock_acquire(&inode->i_lock);
	links = inode->i_links;
	spinlock_release(&inode->i_lock);
	return links;
}

static uint32_t tmpfs_inode_inum(struct tmpfs_inode *inode)
{
	uint32_t inum;
	spinlock_acquire(&inode->i_lock);
	inum = inode->i_inum;
	spinlock_release(&inode->i_lock);
	return inum;
}

static void tmpfs_inode_incr_links(struct tmpfs_inode *inode)
{
	spinlock_acquire(&inode->i_lock);
	++inode->i_links;
	spinlock_release(&inode->i_lock);
}

static void tmpfs_inode_decr_links(struct tmpfs_inode *inode)
{
	spinlock_acquire(&inode->i_lock);
	if (inode->i_links > 0)
		--inode->i_links;
	spinlock_release(&inode->i_lock);
}

static int __tmpfs_dir_lookup(struct inode *dir_inode, struct dentry *dentry)
{
	struct tmpfs_node *dir_tnode;
	struct tmpfs_node *tnode;
	struct inode *inode;

	assert(sleeplock_holding(&dir_inode->i_lock));

	dir_tnode = dir_inode->i_private;
	if (!(dir_inode->i_mode & S_IFDIR))
		return -EINVAL;

	tnode = tmpfs_dir_find_entry(dir_tnode, dentry->d_name);
	if (!tnode)
		return -ENOENT;

	inode = inode_alloc();
	if (!inode)
		return -ENOMEM;

	inode->i_ops = &tmpfs_iops;
	inode->i_fops = &tmpfs_fops;

	sleeplock_acquire(&inode->i_lock);
	inode->i_sb = sblock_dup(dir_inode->i_sb);
	tmpfs_read_inode(inode, tnode);
	sleeplock_release(&inode->i_lock);

	dentry->d_ops = &tmpfs_dops;
	dentry->d_inode = inode;
	inode->i_dentry = dentry;

	inode_add(inode);

	return 0;
}

static void tmpfs_deinit_sb(struct super_block *sb)
{
	struct tmpfs_node *root;

	root = sb->s_private;
	spinlock_acquire(&tmpfs_lock);
	list_del(&root->n_root);
	spinlock_release(&tmpfs_lock);
	tmpfs_destroy_tree(root);
}

static int tmpfs_mount(struct file_system_type *fs_type, const char *dev_name,
		       const char *mnt_point, unsigned long flags,
		       struct dentry **mnt_root)
{
	struct dentry *mnt_dp;
	struct inode *mnt_ip;
	struct super_block *mnt_sb;
	struct tmpfs_node *mnt_tnode;
	struct tmpfs_inode *mnt_tinode;
	const char *mnt_dname = NULL;
	size_t mnt_dname_len = 0;

	mnt_sb = sblock_alloc();
	if (!mnt_sb)
		return -ENOMEM;

	mnt_sb->s_fs_type = &tmpfs_fs_type;
	mnt_sb->s_ops = &tmpfs_sops;
	mnt_sb->s_dev = 0;
	mnt_sb->s_block_size = 8;
	mnt_sb->s_magic = 1;
	path_get_last(mnt_point, &mnt_dname, &mnt_dname_len);
	mnt_dp = dentry_alloc(mnt_dname, mnt_dname_len);
	if (!mnt_dp) {
		sblock_free(mnt_sb);
		return -ENOMEM;
	}
	mnt_sb->s_root = mnt_dp;

	mnt_ip = inode_alloc();
	if (!mnt_ip) {
		dentry_free(mnt_dp);
		sblock_free(mnt_sb);
		return -ENOMEM;
	}

	mnt_dp->d_ops = &tmpfs_dops;
	mnt_dp->d_flags |= DENTRY_MOUNTED;
	mnt_dp->d_inode = mnt_ip;
	mnt_ip->i_dentry = mnt_dp;

	mnt_tnode = tmpfs_node_alloc(mnt_dname, mnt_dname_len);
	if (!mnt_tnode) {
		inode_free(mnt_ip);
		dentry_free(mnt_dp);
		sblock_free(mnt_sb);
		return -ENOMEM;
	}
	mnt_tinode = tmpfs_inode_alloc(0, TMPFS_ROOT, S_IFDIR);
	if (!mnt_tinode) {
		tmpfs_node_free(mnt_tnode);
		inode_free(mnt_ip);
		dentry_free(mnt_dp);
		sblock_free(mnt_sb);
	}
	mnt_tnode->n_inode = mnt_tinode;

	mnt_sb->s_private = mnt_tnode;

	mnt_ip->i_ops = &tmpfs_iops;
	mnt_ip->i_fops = &tmpfs_fops;
	sleeplock_acquire(&mnt_ip->i_lock);
	mnt_ip->i_sb = mnt_sb;
	tmpfs_read_inode(mnt_ip, mnt_tnode);
	sleeplock_release(&mnt_ip->i_lock);

	sblock_add(mnt_sb);
	inode_add(mnt_ip);

	tmpfs_add_root(mnt_tnode);

	*mnt_root = mnt_dp;
	return 0;
}

static void tmpfs_umount(const char *mount_point)
{
}

static void tmpfs_deinit_inode(struct inode *inode)
{
}

static int tmpfs_create(struct inode *dir_inode, struct dentry *new_dentry,
			mode_t mode)
{
	return -EOPNOTSUPP;
}

static int tmpfs_link(struct dentry *old_dentry, struct inode *dir_inode,
		      struct dentry *new_dentry)
{
	struct tmpfs_node *dir_tnode;
	struct tmpfs_node *new_tnode;
	struct tmpfs_node *old_tnode;
	struct tmpfs_inode *old_tinode;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	dir_tnode = dir_inode->i_private;
	if (!(dir_inode->i_mode & S_IFDIR)) {
		ret = -EINVAL;
		goto unlock_and_out;
	}

	new_tnode = tmpfs_node_alloc(new_dentry->d_name,
				     strlen(new_dentry->d_name));
	if (!new_tnode) {
		ret = -ENOMEM;
		goto unlock_and_out;
	}

	sleeplock_acquire(&old_dentry->d_inode->i_lock);
	old_tnode = old_dentry->d_inode->i_private;
	old_tinode = old_tnode->n_inode;
	tmpfs_inode_incr_links(old_tinode);
	sleeplock_release(&old_dentry->d_inode->i_lock);

	new_tnode->n_inode = old_tinode;

	tmpfs_dir_add_entry(dir_tnode, new_tnode);

	ret = __tmpfs_dir_lookup(dir_inode, new_dentry);

unlock_and_out:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int tmpfs_unlink(struct inode *dir_inode, struct dentry *old_dentry)
{
	struct tmpfs_node *dir_tnode;
	struct tmpfs_node *old_tnode;
	struct tmpfs_inode *old_tinode;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	dir_tnode = dir_inode->i_private;
	if (!(dir_inode->i_mode & S_IFDIR)) {
		ret = -EINVAL;
		goto unlock_and_out;
	}

	old_tnode = tmpfs_dir_find_entry(dir_tnode, old_dentry->d_name);
	if (!old_tnode) {
		ret = -ENOENT;
		goto unlock_and_out;
	}

	old_tinode = old_tnode->n_inode;
	tmpfs_inode_decr_links(old_tinode);
	if (tmpfs_inode_links(old_tinode) == 0)
		tmpfs_inode_free(old_tinode);

	tmpfs_dir_del_entry(old_tnode);
	tmpfs_node_free(old_tnode);

unlock_and_out:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int tmpfs_mkdir(struct inode *dir_inode, struct dentry *new_dentry,
		       mode_t mode)
{
	struct tmpfs_node *dir_tnode;
	struct tmpfs_node *new_tnode;
	struct tmpfs_inode *new_tinode;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	dir_tnode = dir_inode->i_private;
	if (!(dir_inode->i_mode & S_IFDIR)) {
		ret = -EINVAL;
		goto unlock_and_out;
	}

	new_tnode = tmpfs_node_alloc(new_dentry->d_name,
				     strlen(new_dentry->d_name));
	if (!new_tnode) {
		ret = -ENOMEM;
		goto unlock_and_out;
	}
	new_tinode = tmpfs_inode_alloc(0, 0, S_IFDIR);
	if (!new_tinode) {
		tmpfs_node_free(new_tnode);
		ret = -ENOMEM;
		goto unlock_and_out;
	}

	tmpfs_dir_add_entry(dir_tnode, new_tnode);

	ret = __tmpfs_dir_lookup(dir_inode, new_dentry);

unlock_and_out:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int tmpfs_rmdir(struct inode *dir_inode, struct dentry *old_dentry)
{
	struct tmpfs_node *dir_tnode;
	struct tmpfs_node *old_tnode;
	struct tmpfs_inode *old_tinode;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	dir_tnode = dir_inode->i_private;
	if (!(dir_inode->i_mode & S_IFDIR)) {
		ret = -EINVAL;
		goto unlock_and_out;
	}

	old_tnode = tmpfs_dir_find_entry(dir_tnode, old_dentry->d_name);
	if (!old_tnode) {
		ret = -ENOENT;
		goto unlock_and_out;
	}

	old_tinode = old_tnode->n_inode;
	if (!(tmpfs_inode_mode(old_tinode) & S_IFDIR)) {
		ret = -EINVAL;
		goto unlock_and_out;
	}

	if (!list_empty(&old_tnode->n_subnodes)) {
		ret = -ENOTEMPTY;
		goto unlock_and_out;
	}

	tmpfs_inode_decr_links(old_tinode);
	if (tmpfs_inode_links(old_tinode) == 0)
		tmpfs_inode_free(old_tinode);

	tmpfs_dir_del_entry(old_tnode);
	tmpfs_node_free(old_tnode);

unlock_and_out:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int tmpfs_lookup(struct inode *dir_inode, struct dentry *dentry)
{
	int ret;

	sleeplock_acquire(&dir_inode->i_lock);
	ret = __tmpfs_dir_lookup(dir_inode, dentry);
	sleeplock_release(&dir_inode->i_lock);

	return ret;
}

static int tmpfs_mknod(struct inode *dir_inode, struct dentry *new_dentry,
		       mode_t mode, dev_t dev)
{
	struct tmpfs_node *new_tnode;
	struct tmpfs_inode *new_tinode;
	struct tmpfs_node *dir_tnode;
	int ret = 0;

	sleeplock_acquire(&dir_inode->i_lock);

	dir_tnode = dir_inode->i_private;
	if (!(dir_inode->i_mode & S_IFDIR)) {
		ret = -EINVAL;
		goto unlock_and_out;
	}

	new_tnode = tmpfs_node_alloc(new_dentry->d_name,
				     strlen(new_dentry->d_name));
	if (!new_tnode) {
		ret = -ENOMEM;
		goto unlock_and_out;
	}
	new_tinode = tmpfs_inode_alloc(dev, 0, mode);
	if (!new_tinode) {
		tmpfs_node_free(new_tnode);
		ret = -ENOMEM;
		goto unlock_and_out;
	}
	new_tnode->n_inode = new_tinode;

	tmpfs_dir_add_entry(dir_tnode, new_tnode);

	ret = __tmpfs_dir_lookup(dir_inode, new_dentry);

unlock_and_out:
	sleeplock_release(&dir_inode->i_lock);
	return ret;
}

static int __tmpfs_readdir(struct inode *inode, void *buf, size_t size,
			   off_t *offset, size_t *rcnt)
{
	struct dirent64 *de64 = NULL;
	struct tmpfs_node *info = inode->i_private;
	off_t off = 0;
	uint16_t reclen = 0;
	size_t r = 0;
	uint64_t p = (uint64_t)buf;
	int ret = 0;
	struct tmpfs_node *curr;
	size_t name_len;

	list_for_each_entry(curr, &info->n_subnodes, n_sub) {
		name_len = strlen(curr->n_name);
		reclen = DIRENT64_NAME_OFFSET + name_len + 1;
		if (r + reclen > size)
			break;

		de64 = (struct dirent64 *)p;
		de64->d_ino = tmpfs_inode_inum(curr->n_inode);
		de64->d_off = off;
		de64->d_reclen = reclen;
		de64->d_type = 0;
		memcpy(de64->d_name, curr->n_name, name_len + 1);

		off += sizeof(struct tmpfs_node);
		p += reclen;
		r += reclen;
	}

	if (rcnt)
		*rcnt = r;

	return ret;
}

static int tmpfs_fread(struct file *file, void *buf, size_t n, off_t *offset,
		       size_t *rcnt)
{
	struct inode *inode = file->f_inode;
	int ret = 0;

	sleeplock_acquire(&inode->i_lock);

	if (!(inode->i_mode & S_IFDIR)) {
		ret = -EOPNOTSUPP;
		goto unlock_and_out;
	}

	ret = __tmpfs_readdir(inode, buf, n, offset, rcnt);

unlock_and_out:
	sleeplock_release(&inode->i_lock);
	return ret;
}

static int tmpfs_fwrite(struct file *file, const void *buf, size_t n,
			off_t *offset, size_t *wcnt)
{
	return -EOPNOTSUPP;
}

static off_t tmpfs_fseek(struct file *file, off_t offset, int whence)
{
	return -EOPNOTSUPP;
}

static int tmpfs_fstat(struct file *file, struct stat *st)
{
	if (!file->f_inode)
		return -EINVAL;

	memset(st, 0, sizeof(*st));

	sleeplock_acquire(&file->f_inode->i_lock);
	st->st_dev = file->f_inode->i_sb->s_dev;
	st->st_ino = file->f_inode->i_num;
	st->st_mode = file->f_inode->i_mode;
	st->st_nlink = file->f_inode->i_links;
	st->st_rdev = file->f_inode->i_dev;
	st->st_size = file->f_inode->i_size;
	st->st_blksize = file->f_inode->i_sb->s_block_size;
	st->st_blocks = file->f_inode->i_size / st->st_blksize;
	sleeplock_release(&file->f_inode->i_lock);

	return 0;
}

static int tmpfs_fopen(struct file *file, struct inode *inode, int flags)
{
	int ret = 0;
	sleeplock_acquire(&inode->i_lock);
	if (IS_CHRDEV(inode->i_mode)) {
		file->f_dev = inode->i_dev;
		file->f_inode = inode_dup(inode);
		file->f_ops = &chrdev_fops;
	} else if (IS_BLKDEV(inode->i_mode)) {
		file->f_dev = inode->i_dev;
		file->f_inode = inode_dup(inode);
		file->f_ops = &blkdev_fops;
	} else {
		ret = -EOPNOTSUPP;
	}
	sleeplock_release(&inode->i_lock);
	return ret;
}

static int tmpfs_ftruncate(struct file *file, off_t len)
{
	return -EOPNOTSUPP;
}

static int tmpfs_dentry_compare(struct dentry *dentry, const char *name,
				size_t len)
{
	return strncmp(dentry->d_name, name, len);
}

struct dentry_operations tmpfs_dops = {
	.compare = tmpfs_dentry_compare,
};

struct super_operations tmpfs_sops = {
	.deinit_inode = tmpfs_deinit_inode,
};

struct inode_operations tmpfs_iops = {
	.create = tmpfs_create,
	.link = tmpfs_link,
	.unlink = tmpfs_unlink,
	.mkdir = tmpfs_mkdir,
	.rmdir = tmpfs_rmdir,
	.lookup = tmpfs_lookup,
	.mknod = tmpfs_mknod,
};

struct file_operations tmpfs_fops = {
	.read = tmpfs_fread,
	.write = tmpfs_fwrite,
	.seek = tmpfs_fseek,
	.stat = tmpfs_fstat,
	.open = tmpfs_fopen,
	.truncate = tmpfs_ftruncate,
};

struct file_system_type tmpfs_fs_type = {
	.name = "tmpfs",
	.deinit_sb = tmpfs_deinit_sb,
	.mount = tmpfs_mount,
	.umount = tmpfs_umount,
};
