#include "tmpfs.h"
#include <asm/page.h>
#include <brk/fs/dcache.h>
#include <brk/fs/fs.h>
#include <brk/fs/path.h>
#include <brk/kernel/ktime.h>
#include <brk/kernel/printk.h>
#include <brk/lib/assert.h>
#include <brk/lib/error.h>
#include <brk/lib/kernel.h>
#include <brk/lib/list.h>
#include <brk/lib/string.h>
#include <brk/lib/types.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/mm_types.h>
#include <brk/mm/pgalloc.h>
#include <uapi/brk/errno.h>
#include <uapi/dirent.h>
#include <uapi/stat.h>
#include <uapi/time.h>

static void tmpfs_stamp_times(struct tmpfs_inode *ip)
{
	struct timespec ts;
	ktime_get_real_ts(&ts);

	ip->i_atime = ts.tv_sec;
	ip->i_atime_nsec = ts.tv_nsec;
	ip->i_mtime = ts.tv_sec;
	ip->i_mtime_nsec = ts.tv_nsec;
	ip->i_ctime = ts.tv_sec;
	ip->i_ctime_nsec = ts.tv_nsec;
}

static void tmpfs_inode_times_to_vfs(const struct tmpfs_inode *t,
				     struct fs_inode *inode)
{
	inode->atime.tv_sec = t->i_atime;
	inode->atime.tv_nsec = t->i_atime_nsec;
	inode->mtime.tv_sec = t->i_mtime;
	inode->mtime.tv_nsec = t->i_mtime_nsec;
	inode->ctime.tv_sec = t->i_ctime;
	inode->ctime.tv_nsec = t->i_ctime_nsec;
}

static void tmpfs_vfs_times_to_inode(const struct fs_inode *inode,
				     struct tmpfs_inode *t)
{
	t->i_atime = inode->atime.tv_sec;
	t->i_atime_nsec = inode->atime.tv_nsec;
	t->i_mtime = inode->mtime.tv_sec;
	t->i_mtime_nsec = inode->mtime.tv_nsec;
	t->i_ctime = inode->ctime.tv_sec;
	t->i_ctime_nsec = inode->ctime.tv_nsec;
}

static struct page *tmpfs_inode_data_page(struct tmpfs_inode *inode, off_t pos)
{
	struct page *pg = inode->i_page;

	if (!pg) {
		pg = page_zalloc(0);
		if (!pg)
			return NULL;
		pg->tmpfs_next = NULL;
		inode->i_page = pg;
	}

	for (off_t i = 0, j = pos / PAGE_SIZE; i < j; ++i) {
		if (pg->tmpfs_next) {
			pg = pg->tmpfs_next;
		} else {
			struct page *new_pg = page_zalloc(0);
			if (!new_pg)
				return NULL;
			new_pg->tmpfs_next = NULL;
			pg->tmpfs_next = new_pg;
			pg = new_pg;
		}
	}

	return pg;
}

static struct page *tmpfs_inode_data_page_ro(struct tmpfs_inode *inode,
					     off_t pos)
{
	struct page *pg = inode->i_page;

	for (off_t i = 0, j = pos / PAGE_SIZE; pg && i < j; ++i)
		pg = pg->tmpfs_next;

	return pg;
}

static struct tmpfs_inode *tmpfs_inode_get(struct tmpfs_super_block *sb,
					   u32 ino)
{
	struct page *pg;
	struct tmpfs_inode *ip;

	ASSERT(ino > 0);

	--ino;

	if (ino >= sb->s_icnt)
		return NULL;

	pg = sb->s_imap;
	for (usize_t i = 0, j = ino / TMPFS_INODE_PER_PAGE; i < j; ++i)
		pg = pg->tmpfs_next;

	ip = (struct tmpfs_inode *)page_to_virt(pg) +
	     (ino % TMPFS_INODE_PER_PAGE);

	return ip;
}

static struct tmpfs_inode *tmpfs_inode_alloc(struct tmpfs_super_block *sb,
					     umode_t mode, dev_t rdev)
{
	struct tmpfs_inode *imap;
	struct page **pg, *new_pg;
	u32 ino = 1;

	pg = &sb->s_imap;

again:
	for (; *pg; pg = &((*pg)->tmpfs_next)) {
		imap = (struct tmpfs_inode *)page_to_virt(*pg);
		for (usize_t i = 0; i < TMPFS_INODE_PER_PAGE; ++i) {
			if (imap[i].i_no == 0) {
				imap[i].i_page = NULL;
				imap[i].i_size = 0;
				imap[i].i_no = ino;
				imap[i].i_mode = mode;
				imap[i].i_nlink = 1;
				imap[i].i_rdev = rdev;
				tmpfs_stamp_times(&imap[i]);
				return &imap[i];
			}
			++ino;
		}
	}

	new_pg = page_zalloc(0);
	if (!new_pg)
		return NULL;
	new_pg->tmpfs_next = NULL;
	*pg = new_pg;
	sb->s_icnt += TMPFS_INODE_PER_PAGE;
	goto again;
}

static void tmpfs_inode_free_data_pages(struct tmpfs_inode *inode)
{
	struct page *curr, *next;

	curr = inode->i_page;
	while (curr) {
		next = curr->tmpfs_next;
		page_free(curr, 0);
		curr = next;
	}
	memset(inode, 0, sizeof(*inode));
}

static void tmpfs_inode_free(struct tmpfs_super_block *sb, u32 ino)
{
	struct tmpfs_inode *ip;

	ip = tmpfs_inode_get(sb, ino);
	if (ip)
		tmpfs_inode_free_data_pages(ip);
}

static struct tmpfs_inode *tmpfs_inode_alloc_dir(struct tmpfs_super_block *sb,
						 umode_t mode)
{
	struct tmpfs_inode *ip;
	struct page *pg;
	struct tmpfs_dir_entry *ent;

	mode |= S_IFDIR;
	ip = tmpfs_inode_alloc(sb, mode, 0);
	if (!ip)
		return NULL;

	pg = page_zalloc(0);
	if (!pg) {
		tmpfs_inode_free(sb, ip->i_no);
		return NULL;
	}
	pg->tmpfs_next = NULL;
	ip->i_page = pg;
	ip->i_size = PAGE_SIZE;

	ent = (struct tmpfs_dir_entry *)page_to_virt(pg);
	ent->d_ino = ip->i_no;
	ent->d_off = 0;
	ent->d_entry_len = PAGE_SIZE;
	ent->d_name_len = 1;
	ent->d_type = DT_DIR;
	ent->d_name[0] = '.';
	ip->i_nlink += 1;

	return ip;
}

static int tmpfs_inode_read(struct tmpfs_super_block *sb,
			    struct fs_inode *inode)
{
	struct tmpfs_inode *t_inode;

	t_inode = tmpfs_inode_get(sb, inode->ino);
	if (!t_inode)
		return -ENOENT;

	inode->mode = t_inode->i_mode;
	inode->nlink = t_inode->i_nlink;
	inode->size = t_inode->i_size;
	inode->rdev = t_inode->i_rdev;
	inode->private_data = t_inode; /* Cache the inode */
	tmpfs_inode_times_to_vfs(t_inode, inode);

	return 0;
}

static int tmpfs_dir_add_entry_to(struct tmpfs_dir_entry *entries, u32 ino,
				  const char *name, usize_t name_len, u8 type,
				  s32 *pos)
{
	u16 n = PAGE_SIZE;
	u16 new_ent_min_len = round_up(20 + name_len, 4);
	struct tmpfs_dir_entry *new_ent;
	struct tmpfs_dir_entry *ent = entries;
	u16 ent_len, ent_min_len;

	while (n >= TMPFS_DIR_ENT_MIN_LEN) {
		ent_len = ent->d_entry_len;

		if (ent->d_ino == 0 && ent_len >= new_ent_min_len) {
			new_ent = ent;
			new_ent->d_ino = ino;
			new_ent->d_off = *pos;
			new_ent->d_name_len = name_len;
			new_ent->d_type = type;
			memcpy(new_ent->d_name, name, name_len);
			return 0;
		}

		ent_min_len = round_up(20 + ent->d_name_len, 4);
		if (ent->d_ino > 0 &&
		    ent_len - ent_min_len >= new_ent_min_len) {
			ent->d_entry_len = ent_min_len;
			new_ent = (struct tmpfs_dir_entry *)((u8 *)ent +
							     ent_min_len);
			new_ent->d_entry_len = ent_len - ent_min_len;
			new_ent->d_ino = ino;
			new_ent->d_off = *pos + ent_min_len;
			new_ent->d_name_len = name_len;
			new_ent->d_type = type;
			memcpy(new_ent->d_name, name, name_len);
			return 0;
		}

		*pos += ent_len;
		ent = (struct tmpfs_dir_entry *)((u8 *)ent + ent_len);
		n -= ent_len;
	}

	return -ENOMEM;
}

static int tmpfs_dir_del_entry_from(struct tmpfs_dir_entry *entries,
				    unsigned int name_len, const char *name)
{
	struct tmpfs_dir_entry *curr = entries;
	struct tmpfs_dir_entry *prev = NULL;
	u16 n = PAGE_SIZE;
	u16 ent_len;

	while (n >= TMPFS_DIR_ENT_MIN_LEN) {
		ent_len = curr->d_entry_len;
		if (curr->d_ino > 0 && curr->d_name_len == name_len &&
		    !memcmp(curr->d_name, name, name_len)) {
			if (prev)
				prev->d_entry_len += ent_len;
			else {
				curr->d_ino = 0;
				curr->d_name_len = 0;
				curr->d_type = DT_UNKNOWN;
			}
			return 0;
		}

		prev = curr;
		n -= ent_len;
		curr = (struct tmpfs_dir_entry *)((u8 *)curr + ent_len);
	}

	return -ENOENT;
}

static u8 tmpfs_imode_to_dt(umode_t mode)
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

static int tmpfs_dir_add_entry(struct tmpfs_inode *dir,
			       struct tmpfs_inode *entry, unsigned int name_len,
			       const char *name)
{
	struct page **pg, *new_pg;
	struct tmpfs_dir_entry *entries;
	u8 dt;
	s32 pos = 0;

	dt = tmpfs_imode_to_dt(entry->i_mode);
	if (dt == DT_UNKNOWN)
		return -EINVAL;

	pg = &dir->i_page;

again:
	for (; *pg; pg = &((*pg)->tmpfs_next)) {
		entries = (struct tmpfs_dir_entry *)page_to_virt(*pg);
		if (tmpfs_dir_add_entry_to(entries, entry->i_no, name, name_len,
					   dt, &pos) == 0)
			return 0;
	}

	new_pg = page_zalloc(0);
	if (!new_pg)
		return -ENOMEM;
	new_pg->tmpfs_next = NULL;
	*pg = new_pg;
	dir->i_size += PAGE_SIZE;
	entries = (struct tmpfs_dir_entry *)page_to_virt(new_pg);
	entries[0].d_ino = 0;
	goto again;
}

static int tmpfs_dir_del_entry(struct tmpfs_inode *dir, unsigned int name_len,
			       const char *name)
{
	struct page **pg;
	struct tmpfs_dir_entry *entries;

	pg = &dir->i_page;
	for (; *pg; pg = &((*pg)->tmpfs_next)) {
		entries = (struct tmpfs_dir_entry *)page_to_virt(*pg);
		if (tmpfs_dir_del_entry_from(entries, name_len, name) == 0)
			return 0;
	}

	return -ENOENT;
}

static struct tmpfs_inode *__tmpfs_dir_lookup(struct tmpfs_super_block *sb,
					      struct tmpfs_dir_entry *entries,
					      const char *name, u32 name_len)
{
	struct tmpfs_dir_entry *ent = entries;
	u16 n = PAGE_SIZE;

	while (n >= TMPFS_DIR_ENT_MIN_LEN) {
		if (ent->d_ino > 0 && ent->d_name_len == name_len &&
		    !memcmp(ent->d_name, name, name_len))
			return tmpfs_inode_get(sb, ent->d_ino);

		n -= ent->d_entry_len;
		ent = (struct tmpfs_dir_entry *)((u8 *)ent + ent->d_entry_len);
	}

	return NULL;
}

static struct tmpfs_inode *tmpfs_dir_lookup(struct tmpfs_super_block *sb,
					    struct tmpfs_inode *dir,
					    const char *name, u32 name_len)
{
	struct page *pg;
	struct tmpfs_dir_entry *entries;
	struct tmpfs_inode *inode;

	for (pg = dir->i_page; pg; pg = pg->tmpfs_next) {
		entries = (struct tmpfs_dir_entry *)page_to_virt(pg);
		inode = __tmpfs_dir_lookup(sb, entries, name, name_len);
		if (inode)
			return inode;
	}

	return NULL;
}

static void tmpfs_free_super(struct tmpfs_super_block *sb);

static struct tmpfs_super_block *tmpfs_alloc_super(void)
{
	struct tmpfs_super_block *sb;
	struct tmpfs_inode *root_ip;
	int err;

	sb = kzalloc(sizeof(*sb));
	if (!sb)
		return NULL;
	sb->s_imap = page_zalloc(0);
	if (!sb->s_imap) {
		kfree(sb);
		return NULL;
	}
	sb->s_icnt = TMPFS_INODE_PER_PAGE;
	sleeplock_init(&sb->s_lock, "tmpfs_super_block.s_lock");

	root_ip = tmpfs_inode_alloc_dir(sb, 0755);
	if (!root_ip) {
		tmpfs_free_super(sb);
		return NULL;
	}
	err = tmpfs_dir_add_entry(root_ip, root_ip, 2, "..");
	if (err) {
		tmpfs_free_super(sb);
		return NULL;
	}

	return sb;
}

static void tmpfs_free_super(struct tmpfs_super_block *sb)
{
	struct tmpfs_inode *imap;
	struct page *curr, *next;

	curr = sb->s_imap;
	while (curr) {
		next = curr->tmpfs_next;
		imap = (struct tmpfs_inode *)page_to_virt(curr);
		for (usize_t i = 0; i < TMPFS_INODE_PER_PAGE; ++i) {
			if (imap[i].i_nlink > 0)
				tmpfs_inode_free_data_pages(&imap[i]);
		}
		page_free(curr, 0);
		curr = next;
	}

	kfree(sb);
}

static int tmpfs_read_file_at(struct tmpfs_inode *inode, void *buf, usize_t n,
			      off_t *pos, usize_t *wcnt)
{
	usize_t size, target, start;
	struct page *pg;
	u8 *data, *b;
	off_t off = *pos;

	if (off >= inode->i_size) {
		if (wcnt)
			*wcnt = 0;
		return 0;
	}

	pg = tmpfs_inode_data_page_ro(inode, off);
	if (!pg)
		return -EIO;

	target = n;
	b = buf;
	while (1) {
		data = (u8 *)page_to_virt(pg);
		size = inode->i_size - off;
		start = off % PAGE_SIZE;
		if (size > PAGE_SIZE - start)
			size = PAGE_SIZE - start;
		if (size > n)
			size = n;
		memcpy(b, data + start, size);

		n -= size;
		off += size;
		pg = pg->tmpfs_next;

		if (n == 0 || !pg)
			break;
	}

	if (wcnt)
		*wcnt = target - n;
	*pos = off;

	return 0;
}

static int tmpfs_write_file_at(struct tmpfs_inode *ip, const void *buf,
			       usize_t n, off_t *pos, usize_t *rcnt)
{
	struct page *pg, *new_pg;
	usize_t target = n, size, start;
	off_t off = *pos;
	u8 *data;
	const u8 *b = buf;
	int ret = 0;

	pg = tmpfs_inode_data_page(ip, off);
	if (!pg)
		return -ENOMEM;

	while (n > 0) {
		start = off % PAGE_SIZE;
		size = PAGE_SIZE - start;
		if (size > n)
			size = n;
		data = (u8 *)page_to_virt(pg);
		memcpy(data + start, b, size);
		n -= size;
		off += size;
		b += size;
		if (n == 0)
			break;
		if (!pg->tmpfs_next) {
			new_pg = page_alloc(0);
			if (!new_pg) {
				ret = -ENOMEM;
				break;
			}
			new_pg->tmpfs_next = NULL;
			pg->tmpfs_next = new_pg;
			pg = new_pg;
		} else {
			pg = pg->tmpfs_next;
		}
	}

	if (rcnt)
		*rcnt = target - n;
	*pos = off;

	return ret;
}

static void tmpfs_inode_attach_fops(struct fs_inode *inode)
{
	if (S_ISCHR(inode->mode))
		inode->fops = &chrdev_fops;
	else if (S_ISBLK(inode->mode))
		inode->fops = &blkdev_fops;
	else if (S_ISDIR(inode->mode))
		inode->fops = &tmpfs_dir_fops;
	else
		inode->fops = &tmpfs_file_fops;
}

static int tmpfs_init_new_inode(struct tmpfs_super_block *sb,
				struct fs_inode *inode)
{
	int err;

	inode->ops = &tmpfs_iops;
	err = tmpfs_inode_read(sb, inode);
	if (err)
		return err;
	tmpfs_inode_attach_fops(inode);
	fs_inode_unlock_new(inode);
	return 0;
}

int tmpfs_mount(struct fs_mount_args *args, struct fs_mount_result *result)
{
	struct fs_super_block *sb;
	struct tmpfs_super_block *t_sb;
	struct fs_inode *root_inode;
	struct fs_dentry *root_dentry;

	sb = fs_super_block_alloc(args->driver);
	if (!sb)
		return -ENOMEM;

	sb->block_size = PAGE_SIZE;
	sb->magic = TMPFS_MAGIC;
	sb->flags = args->flags;
	sb->ops = &tmpfs_sops;
	sb->default_dops = &generic_dop;

	t_sb = tmpfs_alloc_super();
	if (!t_sb) {
		fs_super_block_free(sb);
		return -ENOMEM;
	}
	sb->private_data = t_sb;

	root_inode = fs_inode_get_locked(sb, TMPFS_ROOT_INO);
	if (!root_inode) {
		tmpfs_free_super(t_sb);
		fs_super_block_free(sb);
		return -ENOMEM;
	}
	if (tmpfs_init_new_inode(t_sb, root_inode)) {
		fs_inode_put(root_inode);
		tmpfs_free_super(t_sb);
		fs_super_block_free(sb);
		return -EIO;
	}

	root_dentry = fs_dentry_make_root(root_inode);
	if (!root_dentry) {
		fs_inode_put(root_inode);
		tmpfs_free_super(t_sb);
		fs_super_block_free(sb);
		return -ENOMEM;
	}

	sb->root = fs_dentry_get(root_dentry);

	spinlock_acquire(&args->driver->lock);
	list_add_tail(&sb->instance, &args->driver->super_blocks);
	spinlock_release(&args->driver->lock);

	result->root = root_dentry;
	result->sb = sb;

	return 0;
}

static int tmpfs_write_inode(struct fs_inode *inode, int sync)
{
	(void)sync;

	struct tmpfs_inode *t_inode = inode->private_data;
	struct tmpfs_super_block *t_sb = inode->sb->private_data;

	sleeplock_acquire(&t_sb->s_lock);
	t_inode->i_nlink = inode->nlink;
	t_inode->i_mode = inode->mode;
	t_inode->i_size = inode->size;
	tmpfs_vfs_times_to_inode(inode, t_inode);
	sleeplock_release(&t_sb->s_lock);

	return 0;
}

static void tmpfs_evict_inode(struct fs_inode *inode)
{
	struct tmpfs_inode *t_inode = inode->private_data;
	struct tmpfs_super_block *t_sb = inode->sb->private_data;

	sleeplock_acquire(&t_sb->s_lock);
	if (inode->nlink > 0) {
		t_inode->i_nlink = inode->nlink;
		t_inode->i_mode = inode->mode;
		t_inode->i_size = inode->size;
		tmpfs_vfs_times_to_inode(inode, t_inode);
	} else {
		tmpfs_inode_free(t_sb, t_inode->i_no);
	}
	sleeplock_release(&t_sb->s_lock);
}

static void tmpfs_put_super(struct fs_super_block *sb)
{
	struct tmpfs_super_block *t_sb = sb->private_data;
	struct fs_driver *driver = sb->driver;

	spinlock_acquire(&driver->lock);
	list_del(&sb->instance);
	spinlock_release(&driver->lock);

	fs_dentry_put(sb->root);
	sb->root = NULL;

	tmpfs_free_super(t_sb);
	sb->private_data = NULL;
}

static struct fs_dentry *
tmpfs_lookup(struct fs_inode *dir, struct fs_dentry *dentry, unsigned int flags)
{
	(void)flags;

	struct fs_inode *inode;
	struct tmpfs_inode *t_dir, *t_inode;
	struct tmpfs_super_block *t_sb;
	struct fs_super_block *sb;
	u32 ino;

	t_dir = dir->private_data;
	sb = dir->sb;
	t_sb = sb->private_data;

	sleeplock_acquire(&t_sb->s_lock);
	t_inode = tmpfs_dir_lookup(t_sb, t_dir, dentry->name.name,
				   dentry->name.len);
	if (!t_inode) {
		sleeplock_release(&t_sb->s_lock);
		return NULL;
	}
	ino = t_inode->i_no;
	sleeplock_release(&t_sb->s_lock);

	inode = fs_inode_get_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	spinlock_acquire(&inode->lock);
	bool is_new = (inode->state & I_NEW) != 0;
	spinlock_release(&inode->lock);
	if (is_new && tmpfs_init_new_inode(t_sb, inode)) {
		fs_inode_put(inode);
		return ERR_PTR(-EIO);
	}

	return fs_dentry_splice_alias(inode, dentry);
}

static int tmpfs_create(struct fs_inode *dir, struct fs_dentry *dentry,
			umode_t mode, bool excl)
{
	(void)excl;

	struct fs_inode *inode;
	struct tmpfs_inode *t_new_inode, *t_dir;
	struct tmpfs_super_block *t_sb;
	struct fs_super_block *sb;
	int err;

	t_dir = dir->private_data;
	sb = dir->sb;
	t_sb = sb->private_data;

	sleeplock_acquire(&t_sb->s_lock);
	mode |= S_IFREG;
	t_new_inode = tmpfs_inode_alloc(t_sb, mode, 0);
	if (!t_new_inode) {
		sleeplock_release(&t_sb->s_lock);
		return -ENOMEM;
	}
	err = tmpfs_dir_add_entry(t_dir, t_new_inode, dentry->name.len,
				  dentry->name.name);
	if (err) {
		tmpfs_inode_free(t_sb, t_new_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return err;
	}
	sleeplock_release(&t_sb->s_lock);

	inode = fs_inode_get_locked(sb, t_new_inode->i_no);
	if (!inode) {
		sleeplock_acquire(&t_sb->s_lock);
		(void)tmpfs_dir_del_entry(t_dir, dentry->name.len,
					  dentry->name.name);
		tmpfs_inode_free(t_sb, t_new_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return -ENOMEM;
	}
	if (tmpfs_init_new_inode(t_sb, inode)) {
		fs_inode_put(inode);
		sleeplock_acquire(&t_sb->s_lock);
		(void)tmpfs_dir_del_entry(t_dir, dentry->name.len,
					  dentry->name.name);
		tmpfs_inode_free(t_sb, t_new_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return -EIO;
	}

	fs_dentry_instantiate(dentry, inode);
	inode_touch_mtime_ctime(dir);

	return 0;
}

static int tmpfs_link(struct fs_dentry *old_dentry, struct fs_inode *dir,
		      struct fs_dentry *new_dentry)
{
	struct fs_inode *old_inode;
	struct tmpfs_inode *t_dir, *t_old_inode;
	struct tmpfs_super_block *t_sb;
	int err;

	t_dir = dir->private_data;
	t_sb = dir->sb->private_data;
	old_inode = old_dentry->inode;
	t_old_inode = old_inode->private_data;
	if (S_ISDIR(old_inode->mode))
		return -EPERM;

	sleeplock_acquire(&t_sb->s_lock);
	err = tmpfs_dir_add_entry(t_dir, t_old_inode, new_dentry->name.len,
				  new_dentry->name.name);
	if (err) {
		sleeplock_release(&t_sb->s_lock);
		return err;
	}
	++t_old_inode->i_nlink;
	++old_inode->nlink;
	sleeplock_release(&t_sb->s_lock);

	fs_dentry_instantiate(new_dentry, old_inode);
	inode_touch_ctime(old_inode);
	inode_touch_mtime_ctime(dir);

	return 0;
}

static int tmpfs_unlink(struct fs_inode *dir, struct fs_dentry *dentry)
{
	struct fs_inode *inode;
	struct tmpfs_inode *t_inode, *t_dir;
	struct tmpfs_super_block *t_sb;

	inode = dentry->inode;
	t_inode = inode->private_data;
	t_dir = dir->private_data;
	t_sb = dir->sb->private_data;
	if (S_ISDIR(inode->mode))
		return -EISDIR;

	sleeplock_acquire(&t_sb->s_lock);
	if (tmpfs_dir_del_entry(t_dir, dentry->name.len, dentry->name.name)) {
		sleeplock_release(&t_sb->s_lock);
		return -ENOENT;
	}
	if (t_inode->i_nlink > 0)
		--t_inode->i_nlink;

	if (inode->nlink > 0)
		--inode->nlink;
	sleeplock_release(&t_sb->s_lock);

	inode_touch_ctime(inode);
	inode_touch_mtime_ctime(dir);

	return 0;
}

static int tmpfs_symlink(struct fs_inode *dir, struct fs_dentry *dentry,
			 const char *symname)
{
	struct fs_inode *inode;
	struct tmpfs_inode *t_inode, *t_dir;
	struct tmpfs_super_block *t_sb;
	off_t pos = 0;
	usize_t len, wcnt;
	u32 ino;
	int err;

	if (!symname)
		return -EINVAL;

	t_dir = dir->private_data;
	t_sb = dir->sb->private_data;
	len = strlen(symname);

	sleeplock_acquire(&t_sb->s_lock);

	t_inode = tmpfs_inode_alloc(t_sb, S_IFLNK | 0777, 0);
	if (!t_inode) {
		sleeplock_release(&t_sb->s_lock);
		return -ENOMEM;
	}

	err = tmpfs_write_file_at(t_inode, symname, len, &pos, &wcnt);
	if (err || wcnt != len) {
		tmpfs_inode_free(t_sb, t_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return err ? err : -EIO;
	}

	err = tmpfs_dir_add_entry(t_dir, t_inode, dentry->name.len,
				  dentry->name.name);
	if (err) {
		tmpfs_inode_free(t_sb, t_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return err;
	}

	ino = t_inode->i_no;
	sleeplock_release(&t_sb->s_lock);

	inode = fs_inode_get_locked(dir->sb, ino);
	if (!inode)
		goto rollback;
	if (tmpfs_init_new_inode(t_sb, inode)) {
		fs_inode_put(inode);
		goto rollback;
	}
	fs_dentry_instantiate(dentry, inode);
	inode_touch_mtime_ctime(dir);

	return 0;

rollback:
	sleeplock_acquire(&t_sb->s_lock);
	(void)tmpfs_dir_del_entry(t_dir, dentry->name.len, dentry->name.name);
	tmpfs_inode_free(t_sb, ino);
	sleeplock_release(&t_sb->s_lock);
	return -ENOMEM;
}

static int tmpfs_readlink(struct fs_dentry *dentry, char *buf, int bufsiz)
{
	struct fs_inode *inode = dentry->inode;
	struct tmpfs_inode *t_inode = inode->private_data;
	struct tmpfs_super_block *t_sb = inode->sb->private_data;
	off_t pos = 0;
	usize_t rcnt = 0;
	int err;

	if (!S_ISLNK(inode->mode))
		return -EINVAL;
	if (bufsiz <= 0)
		return -EINVAL;

	sleeplock_acquire(&t_sb->s_lock);
	err = tmpfs_read_file_at(t_inode, buf, (usize_t)(bufsiz - 1), &pos,
				 &rcnt);
	sleeplock_release(&t_sb->s_lock);
	if (err)
		return err;

	buf[rcnt] = '\0';
	return (int)rcnt;
}

static int tmpfs_mkdir(struct fs_inode *dir, struct fs_dentry *dentry,
		       umode_t mode)
{
	struct fs_inode *inode;
	struct fs_super_block *sb;
	struct tmpfs_inode *t_inode, *t_dir;
	struct tmpfs_super_block *t_sb;
	int err;

	t_dir = dir->private_data;
	sb = dir->sb;
	t_sb = sb->private_data;

	sleeplock_acquire(&t_sb->s_lock);
	t_inode = tmpfs_inode_alloc_dir(t_sb, mode);
	if (!t_inode) {
		sleeplock_release(&t_sb->s_lock);
		return -ENOMEM;
	}
	tmpfs_dir_add_entry(t_inode, t_dir, 2, "..");
	err = tmpfs_dir_add_entry(t_dir, t_inode, dentry->name.len,
				  dentry->name.name);
	if (err) {
		tmpfs_inode_free(t_sb, t_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return err;
	}
	++t_dir->i_nlink;
	++dir->nlink;
	sleeplock_release(&t_sb->s_lock);

	inode = fs_inode_get_locked(sb, t_inode->i_no);
	if (!inode) {
		sleeplock_acquire(&t_sb->s_lock);
		(void)tmpfs_dir_del_entry(t_dir, dentry->name.len,
					  dentry->name.name);
		tmpfs_inode_free(t_sb, t_inode->i_no);
		if (t_dir->i_nlink > 0)
			--t_dir->i_nlink;
		if (dir->nlink > 0)
			--dir->nlink;
		sleeplock_release(&t_sb->s_lock);
		return -ENOMEM;
	}
	if (tmpfs_init_new_inode(t_sb, inode)) {
		fs_inode_put(inode);
		sleeplock_acquire(&t_sb->s_lock);
		(void)tmpfs_dir_del_entry(t_dir, dentry->name.len,
					  dentry->name.name);
		tmpfs_inode_free(t_sb, t_inode->i_no);
		if (t_dir->i_nlink > 0)
			--t_dir->i_nlink;
		if (dir->nlink > 0)
			--dir->nlink;
		sleeplock_release(&t_sb->s_lock);
		return -EIO;
	}
	fs_dentry_instantiate(dentry, inode);
	inode_touch_mtime_ctime(dir);

	return 0;
}

static bool tmpfs_dir_is_empty(struct tmpfs_inode *dir)
{
	struct page *pg;
	struct tmpfs_dir_entry *ent;
	u16 n;

	for (pg = dir->i_page; pg; pg = pg->tmpfs_next) {
		ent = (struct tmpfs_dir_entry *)page_to_virt(pg);
		n = PAGE_SIZE;
		while (n >= TMPFS_DIR_ENT_MIN_LEN) {
			if (ent->d_ino > 0) {
				if (!(ent->d_name_len == 1 &&
				      ent->d_name[0] == '.') &&
				    !(ent->d_name_len == 2 &&
				      ent->d_name[0] == '.' &&
				      ent->d_name[1] == '.'))
					return false;
			}
			n -= ent->d_entry_len;
			ent = (struct tmpfs_dir_entry *)((u8 *)ent +
							 ent->d_entry_len);
		}
	}

	return true;
}

static int tmpfs_rmdir(struct fs_inode *dir, struct fs_dentry *dentry)
{
	struct fs_inode *inode;
	struct tmpfs_inode *t_dir, *t_inode;
	struct tmpfs_super_block *t_sb;
	int err;

	t_dir = dir->private_data;
	inode = dentry->inode;
	t_inode = inode->private_data;
	t_sb = dir->sb->private_data;
	if (!S_ISDIR(inode->mode))
		return -ENOTDIR;

	sleeplock_acquire(&t_sb->s_lock);
	if (!tmpfs_dir_is_empty(t_inode)) {
		sleeplock_release(&t_sb->s_lock);
		return -ENOTEMPTY;
	}

	err = tmpfs_dir_del_entry(t_dir, dentry->name.len, dentry->name.name);
	if (err) {
		sleeplock_release(&t_sb->s_lock);

		return err;
	}

	t_inode->i_nlink = 0;
	--t_dir->i_nlink;
	--dir->nlink;

	sleeplock_release(&t_sb->s_lock);

	inode_touch_ctime(inode);
	inode_touch_mtime_ctime(dir);

	return 0;
}

static int tmpfs_rename(struct fs_inode *old_dir, struct fs_dentry *old_dentry,
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

static int tmpfs_mknod(struct fs_inode *dir, struct fs_dentry *dentry,
		       umode_t mode, dev_t dev)
{
	struct fs_inode *inode;
	struct tmpfs_inode *t_inode, *t_dir;
	struct tmpfs_super_block *t_sb;
	int err;

	t_dir = dir->private_data;
	t_sb = dir->sb->private_data;

	sleeplock_acquire(&t_sb->s_lock);

	t_inode = tmpfs_inode_alloc(t_sb, mode, dev);
	if (!t_inode) {
		sleeplock_release(&t_sb->s_lock);
		return -ENOMEM;
	}
	err = tmpfs_dir_add_entry(t_dir, t_inode, dentry->name.len,
				  dentry->name.name);
	if (err) {
		tmpfs_inode_free(t_sb, t_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return err;
	}
	sleeplock_release(&t_sb->s_lock);

	inode = fs_inode_get_locked(dir->sb, t_inode->i_no);
	if (!inode) {
		sleeplock_acquire(&t_sb->s_lock);
		(void)tmpfs_dir_del_entry(t_dir, dentry->name.len,
					  dentry->name.name);
		tmpfs_inode_free(t_sb, t_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return -ENOMEM;
	}
	if (tmpfs_init_new_inode(t_sb, inode)) {
		fs_inode_put(inode);
		sleeplock_acquire(&t_sb->s_lock);
		(void)tmpfs_dir_del_entry(t_dir, dentry->name.len,
					  dentry->name.name);
		tmpfs_inode_free(t_sb, t_inode->i_no);
		sleeplock_release(&t_sb->s_lock);
		return -EIO;
	}
	fs_dentry_instantiate(dentry, inode);
	inode_touch_mtime_ctime(dir);

	return 0;
}

static int tmpfs_getattr(const struct fs_path *path, struct stat *stat,
			 u32 mask, unsigned int flags)
{
	(void)flags;
	(void)mask;

	struct fs_inode *inode = path->dentry->inode;

	memset(stat, 0, sizeof(*stat));
	stat->st_ino = inode->ino;
	stat->st_mode = inode->mode;
	stat->st_nlink = inode->nlink;
	stat->st_rdev = inode->rdev;
	stat->st_size = inode->size;
	stat->st_blksize = PAGE_SIZE;
	stat->st_blocks = (inode->size + 511) / 512;
	inode_times_to_stat(inode, stat);
	return 0;
}

static int tmpfs_setattr(struct fs_dentry *dentry, struct fs_iattr *attr)
{
	(void)dentry;
	(void)attr;
	return -EOPNOTSUPP;
}

static int tmpfs_file_open(struct fs_inode *inode, struct fs_file *file)
{
	(void)file;
	if (S_ISREG(inode->mode) || S_ISLNK(inode->mode))
		return 0;
	return -EINVAL;
}

static ssize_t tmpfs_file_read(struct fs_file *file, char *buf, usize_t size,
			       loff_t *pos)
{
	struct fs_inode *inode = file->inode;
	struct tmpfs_inode *t_inode = inode->private_data;
	struct tmpfs_super_block *t_sb = inode->sb->private_data;
	int err = 0;
	usize_t rcnt = 0;

	sleeplock_acquire(&inode->rwsem);

	sleeplock_acquire(&t_sb->s_lock);
	err = tmpfs_read_file_at(t_inode, buf, size, pos, &rcnt);
	sleeplock_release(&t_sb->s_lock);

	if (err) {
		sleeplock_release(&inode->rwsem);
		return err;
	}

	sleeplock_release(&inode->rwsem);

	return rcnt;
}

static ssize_t tmpfs_file_write(struct fs_file *file, const char *buf,
				usize_t size, loff_t *pos)
{
	struct fs_inode *inode = file->inode;
	struct tmpfs_inode *t_inode = inode->private_data;
	struct tmpfs_super_block *t_sb = inode->sb->private_data;
	int err = 0;
	usize_t wcnt = 0;

	loff_t old_pos = *pos;

	sleeplock_acquire(&inode->rwsem);

	sleeplock_acquire(&t_sb->s_lock);
	err = tmpfs_write_file_at(t_inode, buf, size, pos, &wcnt);
	sleeplock_release(&t_sb->s_lock);

	if (err) {
		sleeplock_release(&inode->rwsem);
		return err;
	}

	if (old_pos + (loff_t)wcnt > inode->size)
		inode->size = old_pos + (loff_t)wcnt;

	inode_touch_mtime(inode);
	sleeplock_acquire(&t_sb->s_lock);
	t_inode->i_size = (u32)inode->size;
	tmpfs_vfs_times_to_inode(inode, t_inode);
	sleeplock_release(&t_sb->s_lock);

	sleeplock_release(&inode->rwsem);

	return wcnt;
}

static loff_t tmpfs_file_llseek(struct fs_file *file, loff_t offset, int whence)
{
	struct fs_inode *inode = file->inode;
	struct tmpfs_inode *t_inode = inode->private_data;
	struct tmpfs_super_block *t_sb = inode->sb->private_data;
	int err = 0;
	loff_t new_pos = 0;

	if (whence == SEEK_SET)
		new_pos = offset;
	else if (whence == SEEK_CUR)
		new_pos = file->pos + offset;
	else if (whence == SEEK_END)
		new_pos = t_inode->i_size + offset;
	else
		return -EINVAL;

	if (new_pos < 0)
		return -EINVAL;

	sleeplock_acquire(&t_sb->s_lock);
	sleeplock_release(&t_sb->s_lock);
	if (err)
		return err;

	return new_pos;
}

static int tmpfs_file_iterate_shared(struct fs_file *file,
				     struct fs_dir_iterator *ctx)
{
	(void)file;
	(void)ctx;
	return -ENOTDIR;
}

static int tmpfs_file_fsync(struct fs_file *file, loff_t start, loff_t end,
			    int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int tmpfs_file_flush(struct fs_file *file)
{
	(void)file;
	return 0;
}

static long tmpfs_file_ioctl(struct fs_file *file, unsigned int cmd,
			     unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return 0;
}

static int tmpfs_dir_open(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;
	(void)file;
	klog_debug("%s(): Called\n", __func__);
	return 0;
}

static ssize_t tmpfs_dir_read(struct fs_file *file, char *buf, usize_t size,
			      loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static ssize_t tmpfs_dir_write(struct fs_file *file, const char *buf,
			       usize_t size, loff_t *pos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)pos;
	return -EISDIR;
}

static loff_t tmpfs_dir_llseek(struct fs_file *file, loff_t offset, int whence)
{
	loff_t new_pos = 0;

	if (whence == SEEK_SET)
		new_pos = offset;
	else if (whence == SEEK_CUR)
		new_pos = file->pos + offset;
	else
		return -EINVAL;

	if (new_pos < 0)
		return -EINVAL;
	return new_pos;
}

static int tmpfs_dir_iterate_shared(struct fs_file *file,
				    struct fs_dir_iterator *ctx)
{
	struct fs_inode *inode = file->inode;
	struct tmpfs_inode *t_inode = inode->private_data;
	struct tmpfs_super_block *t_sb = inode->sb->private_data;
	struct page *pg;
	struct tmpfs_dir_entry *ent;
	u16 n;
	loff_t remaining;

	sleeplock_acquire(&t_sb->s_lock);
	remaining = ctx->pos;
	for (pg = t_inode->i_page; pg && remaining >= PAGE_SIZE;
	     pg = pg->tmpfs_next)
		remaining -= PAGE_SIZE;

	for (; pg; pg = pg->tmpfs_next) {
		ent = (struct tmpfs_dir_entry *)page_to_virt(pg);
		n = PAGE_SIZE;
		while (n >= TMPFS_DIR_ENT_MIN_LEN) {
			if (remaining > 0) {
				remaining -= ent->d_entry_len;
				ctx->pos += ent->d_entry_len;
				n -= ent->d_entry_len;
				ent = (struct tmpfs_dir_entry
					       *)((u8 *)ent + ent->d_entry_len);
				continue;
			}
			if (ent->d_ino > 0 &&
			    !ctx->actor(ctx, ent->d_name, ent->d_name_len,
					ent->d_off, ent->d_ino, ent->d_type)) {
				sleeplock_release(&t_sb->s_lock);
				return 0;
			}
			ctx->pos += ent->d_entry_len;
			n -= ent->d_entry_len;
			ent = (struct tmpfs_dir_entry *)((u8 *)ent +
							 ent->d_entry_len);
		}
	}
	sleeplock_release(&t_sb->s_lock);

	return 0;
}

static int tmpfs_dir_fsync(struct fs_file *file, loff_t start, loff_t end,
			   int datasync)
{
	(void)file;
	(void)start;
	(void)end;
	(void)datasync;
	return 0;
}

static int tmpfs_dir_flush(struct fs_file *file)
{
	(void)file;
	return 0;
}

static long tmpfs_dir_ioctl(struct fs_file *file, unsigned int cmd,
			    unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return 0;
}

struct fs_driver tmpfs_fs_type = {
	.name = "tmpfs",
	.mount = tmpfs_mount,
	.lock = SPINLOCK_INITIALIZER("tmpfs_fs_lock"),
	.super_blocks = LIST_INITIALIZER(tmpfs_fs_type.super_blocks),
	.list = LIST_INITIALIZER(tmpfs_fs_type.list),
};

const struct fs_super_block_ops tmpfs_sops = {
	.write_inode = tmpfs_write_inode,
	.evict_inode = tmpfs_evict_inode,
	.put_super = tmpfs_put_super,
};

const struct fs_inode_ops tmpfs_iops = {
	.lookup = tmpfs_lookup,
	.create = tmpfs_create,
	.link = tmpfs_link,
	.unlink = tmpfs_unlink,
	.symlink = tmpfs_symlink,
	.readlink = tmpfs_readlink,
	.mkdir = tmpfs_mkdir,
	.rmdir = tmpfs_rmdir,
	.rename = tmpfs_rename,
	.mknod = tmpfs_mknod,
	.getattr = tmpfs_getattr,
	.setattr = tmpfs_setattr,
};

const struct fs_file_ops tmpfs_file_fops = {
	.open = tmpfs_file_open,
	.read = tmpfs_file_read,
	.write = tmpfs_file_write,
	.llseek = tmpfs_file_llseek,
	.iterate_shared = tmpfs_file_iterate_shared,
	.fsync = tmpfs_file_fsync,
	.flush = tmpfs_file_flush,
	.ioctl = tmpfs_file_ioctl,
};

const struct fs_file_ops tmpfs_dir_fops = {
	.open = tmpfs_dir_open,
	.read = tmpfs_dir_read,
	.write = tmpfs_dir_write,
	.llseek = tmpfs_dir_llseek,
	.iterate_shared = tmpfs_dir_iterate_shared,
	.fsync = tmpfs_dir_fsync,
	.flush = tmpfs_dir_flush,
	.ioctl = tmpfs_dir_ioctl,
};
