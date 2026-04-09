#include "internal.h"
#include <brk/align.h>
#include <brk/asm.h>
#include <brk/dcache.h>
#include <brk/dirent.h>
#include <brk/errno.h>
#include <brk/fs.h>
#include <brk/lock.h>
#include <brk/mm_types.h>
#include <brk/path.h>
#include <brk/pgalloc.h>
#include <brk/slab.h>
#include <brk/stat.h>
#include <brk/string.h>
#include <brk/types.h>

static struct page *tmpfs_inode_data_page_unlock(struct tmpfs_inode *ip,
						 off_t off)
{
	off_t which_pg = off / PAGE_SIZE;
	off_t i = 0;
	struct page *pg = ip->i_page, *new_pg;

	if (!pg) {
		pg = page_zalloc(0);
		if (!pg)
			return NULL;
		pg->tmpfs_next = NULL;
		ip->i_page = pg;
	}

	for (; i < which_pg; ++i) {
		if (pg->tmpfs_next) {
			pg = pg->tmpfs_next;
		} else {
			new_pg = page_zalloc(0);
			if (!new_pg)
				return NULL;
			new_pg->tmpfs_next = NULL;
			pg->tmpfs_next = new_pg;
			pg = new_pg;
		}
	}

	return pg;
}

static struct tmpfs_inode *tmpfs_get_inode_unlock(struct tmpfs_super_block *sb,
						  uint32_t ino)
{
	size_t which_pg, i;
	struct page *pg;
	struct tmpfs_inode *ip;

	assert(ino > 0);

	--ino;

	if (ino >= sb->s_icnt)
		return NULL;

	which_pg = ino / TMPFS_INODE_PER_PAGE;
	pg = sb->s_imap;
	for (i = 0; i < which_pg; ++i)
		pg = pg->tmpfs_next;

	ip = (struct tmpfs_inode *)page_to_virt(pg) +
	     (ino % TMPFS_INODE_PER_PAGE);

	return ip;
}

static struct tmpfs_inode *
tmpfs_alloc_inode_unlock(struct tmpfs_super_block *sb, mode_t mode, dev_t rdev)
{
	struct tmpfs_inode *imap;
	struct page **pg, *new_pg;
	uint32_t ino = 1;

	pg = &sb->s_imap;

again:
	for (; *pg; pg = &((*pg)->tmpfs_next)) {
		imap = (struct tmpfs_inode *)page_to_virt(*pg);
		for (size_t i = 0; i < TMPFS_INODE_PER_PAGE; ++i) {
			if (imap[i].i_no == 0) {
				imap[i].i_page = NULL;
				imap[i].i_size = 0;
				imap[i].i_no = ino;
				imap[i].i_mode = mode;
				imap[i].i_nlink = 1;
				imap[i].i_rdev = rdev;
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

static void tmpfs_destroy_inode_unlock(struct tmpfs_inode *ip)
{
	struct page *curr, *next;

	curr = ip->i_page;
	while (curr) {
		next = curr->tmpfs_next;
		page_free(curr, 0);
		curr = next;
	}
	memset(ip, 0, sizeof(*ip));
}

static void tmpfs_free_inode_unlock(struct tmpfs_super_block *sb, uint32_t ino)
{
	struct tmpfs_inode *ip;

	ip = tmpfs_get_inode_unlock(sb, ino);
	if (ip)
		tmpfs_destroy_inode_unlock(ip);
}

static struct tmpfs_inode *
tmpfs_alloc_dir_inode_unlock(struct tmpfs_super_block *sb, mode_t mode)
{
	struct tmpfs_inode *ip;
	struct page *pg;
	struct tmpfs_dir_entry *ent;

	mode |= S_IFDIR;
	ip = tmpfs_alloc_inode_unlock(sb, mode, 0);
	if (!ip)
		return NULL;

	pg = page_zalloc(0);
	if (!pg) {
		tmpfs_free_inode_unlock(sb, ip->i_no);
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

static int __tmpfs_dir_add_entry_unlock(struct tmpfs_dir_entry *ents,
					uint32_t ino, const char *name,
					size_t name_len, uint8_t type,
					int32_t *off)
{
	uint16_t n = PAGE_SIZE;
	uint16_t new_ent_min_len = align_up(20 + name_len, 4);
	struct tmpfs_dir_entry *new_ent;
	struct tmpfs_dir_entry *ent = ents;
	uint16_t ent_len, ent_min_len;

	while (n >= TMPFS_DIR_ENT_MIN_LEN) {
		ent_len = ent->d_entry_len;

		if (ent->d_ino == 0 && ent_len >= new_ent_min_len) {
			new_ent = ent;
			new_ent->d_ino = ino;
			new_ent->d_off = *off;
			new_ent->d_name_len = name_len;
			new_ent->d_type = type;
			memcpy(new_ent->d_name, name, name_len);
			return 0;
		}

		ent_min_len = align_up(20 + ent->d_name_len, 4);
		if (ent->d_ino > 0 &&
		    ent_len - ent_min_len >= new_ent_min_len) {
			ent->d_entry_len = ent_min_len;
			new_ent = (struct tmpfs_dir_entry *)((uint8_t *)ent +
							     ent_min_len);
			new_ent->d_entry_len = ent_len - ent_min_len;
			new_ent->d_ino = ino;
			new_ent->d_off = *off + ent_min_len;
			new_ent->d_name_len = name_len;
			new_ent->d_type = type;
			memcpy(new_ent->d_name, name, name_len);
			return 0;
		}

		*off += ent_len;
		ent = (struct tmpfs_dir_entry *)((uint8_t *)ent + ent_len);
		n -= ent_len;
	}

	return -ENOMEM;
}

static int __tmpfs_dir_del_entry_unlock(struct tmpfs_super_block *sb,
					struct tmpfs_dir_entry *ents,
					const char *name)
{
	struct tmpfs_dir_entry *curr = ents;
	struct tmpfs_dir_entry *prev = NULL;
	uint16_t n = PAGE_SIZE;
	uint16_t ent_len;

	while (n >= TMPFS_DIR_ENT_MIN_LEN) {
		ent_len = curr->d_entry_len;

		if (!strncmp(curr->d_name, name, curr->d_name_len)) {
			tmpfs_free_inode_unlock(sb, curr->d_ino);
			if (prev)
				prev->d_entry_len += ent_len;
			else
				curr->d_ino = 0;
			return 0;
		}

		prev = curr;
		n -= ent_len;
		curr = (struct tmpfs_dir_entry *)((uint8_t *)curr + ent_len);
	}

	return -ENOENT;
}

static uint8_t tmpfs_imode_to_dt(mode_t mode)
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

static int tmpfs_dir_add_entry_unlock(struct tmpfs_inode *dir_ip,
				      struct tmpfs_inode *ent_ip,
				      const char *name)
{
	size_t name_len = strlen(name);
	struct page **pg, *new_pg;
	struct tmpfs_dir_entry *ents;
	uint8_t dt;
	int32_t off = 0;

	dt = tmpfs_imode_to_dt(ent_ip->i_mode);
	if (dt == DT_UNKNOWN)
		return -EINVAL;

	pg = &dir_ip->i_page;

again:
	for (; *pg; pg = &((*pg)->tmpfs_next)) {
		ents = (struct tmpfs_dir_entry *)page_to_virt(*pg);
		if (__tmpfs_dir_add_entry_unlock(ents, ent_ip->i_no, name,
						 name_len, dt, &off) == 0)
			return 0;
	}

	new_pg = page_zalloc(0);
	if (!new_pg)
		return -ENOMEM;
	new_pg->tmpfs_next = NULL;
	*pg = new_pg;
	dir_ip->i_size += PAGE_SIZE;
	ents = (struct tmpfs_dir_entry *)page_to_virt(new_pg);
	ents[0].d_ino = 0;
	goto again;
}

static int tmpfs_dir_del_entry_unlock(struct tmpfs_super_block *sb,
				      struct tmpfs_inode *dir_ip,
				      const char *name)
{
	struct page **pg;
	struct tmpfs_dir_entry *ents;

	pg = &dir_ip->i_page;
	for (; *pg; pg = &((*pg)->tmpfs_next)) {
		ents = (struct tmpfs_dir_entry *)page_to_virt(*pg);
		if (__tmpfs_dir_del_entry_unlock(sb, ents, name) == 0)
			return 0;
	}

	return -ENOENT;
}

static struct tmpfs_inode *
__tmpfs_dir_lookup_unlock(struct tmpfs_super_block *sb,
			  struct tmpfs_dir_entry *ents, const char *name)
{
	struct tmpfs_dir_entry *ent = ents;
	uint16_t n = PAGE_SIZE;

	while (n >= TMPFS_DIR_ENT_MIN_LEN) {
		if (!strncmp(ent->d_name, name, ent->d_name_len))
			return tmpfs_get_inode_unlock(sb, ent->d_ino);

		n -= ent->d_entry_len;
		ent = (struct tmpfs_dir_entry *)((uint8_t *)ent +
						 ent->d_entry_len);
	}

	return NULL;
}

static struct tmpfs_inode *tmpfs_dir_lookup_unlock(struct tmpfs_super_block *sb,
						   struct tmpfs_inode *dir_ip,
						   const char *name)
{
	struct page *pg;
	struct tmpfs_dir_entry *ents;
	struct tmpfs_inode *ip;

	for (pg = dir_ip->i_page; pg; pg = pg->tmpfs_next) {
		ents = (struct tmpfs_dir_entry *)page_to_virt(pg);
		ip = __tmpfs_dir_lookup_unlock(sb, ents, name);
		if (ip)
			return ip;
	}

	return NULL;
}

static struct tmpfs_super_block *tmpfs_alloc_sb_unlock(void)
{
	struct tmpfs_super_block *sb;
	struct tmpfs_inode *root_ip;

	sb = kzalloc(sizeof(*sb));
	if (!sb)
		return NULL;
	sb->s_imap = page_zalloc(0);
	if (!sb->s_imap) {
		kfree(sb);
		return NULL;
	}
	sb->s_icnt = TMPFS_INODE_PER_PAGE;
	spinlock_init(&sb->s_lock, "tmpfs");

	root_ip = tmpfs_alloc_dir_inode_unlock(sb, 0);
	tmpfs_dir_add_entry_unlock(root_ip, root_ip, "..");
	root_ip->i_nlink += 1;

	return sb;
}

static void tmpfs_free_sb_unlock(struct tmpfs_super_block *sb)
{
	struct tmpfs_inode *imap;
	struct page *curr, *next;

	curr = sb->s_imap;
	while (curr) {
		next = curr->tmpfs_next;
		imap = (struct tmpfs_inode *)page_to_virt(curr);
		for (size_t i = 0; i < TMPFS_INODE_PER_PAGE; ++i) {
			if (imap[i].i_nlink > 0)
				tmpfs_destroy_inode_unlock(&imap[i]);
		}
		page_free(curr, 0);
		curr = next;
	}

	kfree(sb);
}

static int tmpfs_read_file_unlock(struct tmpfs_inode *ip, void *buf, size_t n,
				  off_t *offset, size_t *wcnt)
{
	size_t size, target, start;
	struct page *pg;
	uint8_t *data, *b;
	off_t off = *offset;

	if (off >= ip->i_size) {
		if (wcnt)
			*wcnt = 0;
		return 0;
	}

	pg = tmpfs_inode_data_page_unlock(ip, off);
	if (!pg)
		return -ERANGE;

	target = n;
	b = buf;
	while (1) {
		data = (uint8_t *)page_to_virt(pg);
		size = ip->i_size - off;
		start = off % PAGE_SIZE;
		if (size > PAGE_SIZE - start)
			size = PAGE_SIZE - start;
		if (size > n)
			size = n;
		memcpy(b, data + start, size);

		n -= size;

		if (n == 0 || !pg->tmpfs_next)
			break;
		off += size;
		pg = pg->tmpfs_next;
	}

	if (wcnt)
		*wcnt = target - n;
	*offset = off;

	return 0;
}

static void tmpfs_dir_iter_init(struct tmpfs_dir_iter *iter,
				struct tmpfs_inode *ip, off_t offset)
{
	struct page *pg;

	if (offset < ip->i_size) {
		pg = tmpfs_inode_data_page_unlock(ip, offset);
		iter->pg = pg;
		iter->in_pg_off = offset % PAGE_SIZE;
	} else {
		iter->pg = NULL;
		iter->in_pg_off = 0;
	}
}

static struct tmpfs_dir_entry *
__tmpfs_dir_iter_next(struct tmpfs_dir_iter *iter)
{
	struct tmpfs_dir_entry *ent;
	off_t n = PAGE_SIZE - iter->in_pg_off;

	if (!iter->pg)
		return NULL;

	if (n < TMPFS_DIR_ENT_MIN_LEN) {
		if (iter->pg->tmpfs_next) {
			iter->pg = iter->pg->tmpfs_next;
			iter->in_pg_off = 0;
		} else {
			return NULL;
		}
	}

	ent = (struct tmpfs_dir_entry *)(page_to_virt(iter->pg) +
					 iter->in_pg_off);
	iter->in_pg_off += ent->d_entry_len;

	return ent;
}

static struct tmpfs_dir_entry *tmpfs_dir_iter_next(struct tmpfs_dir_iter *iter)
{
	struct tmpfs_dir_entry *ent;

	while (1) {
		ent = __tmpfs_dir_iter_next(iter);
		if (!ent)
			return NULL;
		if (ent->d_ino == 0)
			continue;
		break;
	}
	return ent;
}

static int tmpfs_read_dir_unlock(struct tmpfs_inode *ip, void *buf, size_t n,
				 off_t *offset, size_t *rcnt)
{
	struct tmpfs_dir_iter iter;
	struct tmpfs_dir_entry *ent;
	uint16_t reclen = 0;
	struct dirent64 *de64 = NULL;
	size_t r = 0;
	uint64_t b = (uint64_t)buf;

	tmpfs_dir_iter_init(&iter, ip, *offset);

	while ((ent = tmpfs_dir_iter_next(&iter))) {
		reclen = DIRENT64_NAME_OFFSET + ent->d_name_len + 1;
		reclen = align_up(reclen, alignof(*de64));
		if (r + reclen > n)
			break;
		de64 = (struct dirent64 *)b;
		de64->d_ino = ent->d_ino;
		de64->d_off = ent->d_off;
		de64->d_reclen = reclen;
		de64->d_type = ent->d_type;
		memcpy(de64->d_name, ent->d_name, ent->d_name_len);
		de64->d_name[ent->d_name_len] = '\0';
		b += reclen;
		r += reclen;
		*offset = ent->d_off + ent->d_entry_len;
	}

	if (rcnt)
		*rcnt = r;

	return 0;
}

static int tmpfs_write_file_unlock(struct tmpfs_inode *ip, const void *buf,
				   size_t n, off_t *offset, size_t *rcnt)
{
	struct page *pg, *new_pg;
	size_t target = n, size, start;
	off_t off = *offset;
	uint8_t *data;
	const uint8_t *b = buf;
	int ret = 0;

	pg = tmpfs_inode_data_page_unlock(ip, off);
	if (!pg)
		return -ENOMEM;

	while (n > 0) {
		start = off % PAGE_SIZE;
		size = PAGE_SIZE - start;
		if (size > n)
			size = n;
		data = (uint8_t *)page_to_virt(pg);
		memcpy(data + start, b, size);
		n -= size;
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
		off += size;
		b += size;
	}

	if (rcnt)
		*rcnt = target - n;
	*offset = off;

	return ret;
}

static int tmpfs_truncate_unlock(struct tmpfs_inode *ip, off_t len)
{
	off_t npg = len / PAGE_SIZE;
	struct page *pg = ip->i_page, *next;

	for (off_t i = 0; i < npg; ++i)
		pg = pg->tmpfs_next;

	next = pg->tmpfs_next;
	pg->tmpfs_next = NULL;
	pg = next;
	while (pg) {
		next = pg->tmpfs_next;
		page_free(pg, 0);
		pg = next;
	}

	return 0;
}

static int tmpfs_seek_unlock(struct tmpfs_inode *ip, off_t off)
{
	if (off > ip->i_size) {
		if (!tmpfs_inode_data_page_unlock(ip, off))
			return -ENOMEM;
	}
	return 0;
}

static void tmpfs_deinit_sb(struct super_block *sb)
{
	tmpfs_free_sb_unlock(sb->s_private);
}

static int tmpfs_mount(const struct file_system_type *fs_type,
		       const char *dev_name, const char *mnt_point,
		       unsigned long flags, struct dentry **mnt_root)
{
	struct dentry *dp;
	struct inode *ip;
	struct super_block *sb;
	const char *name = NULL;
	size_t name_len = 0;
	struct tmpfs_super_block *t_sb;
	struct tmpfs_inode *t_ip;

	sb = sblock_alloc();
	if (!sb)
		return -ENOMEM;
	sb->s_fs_type = &tmpfs_fs_type;
	sb->s_ops = &tmpfs_sops;
	sb->s_dev = 0;
	sb->s_block_size = PAGE_SIZE;
	sb->s_magic = TMPFS_MAGIC;
	path_get_last(mnt_point, &name, &name_len);
	dp = dentry_alloc(name, name_len);
	if (!dp) {
		sblock_free(sb);
		return -ENOMEM;
	}
	sb->s_root = dp;

	ip = inode_alloc();
	if (!ip) {
		dentry_free(dp);
		sblock_free(sb);
		return -ENOMEM;
	}

	dp->d_ops = &tmpfs_dops;
	dp->d_flags |= DENTRY_MOUNTED;
	dp->d_inode = ip;

	t_sb = tmpfs_alloc_sb_unlock();
	if (!t_sb) {
		inode_free(ip);
		dentry_free(dp);
		sblock_free(sb);
		return -ENOMEM;
	}
	sb->s_private = t_sb;

	t_ip = tmpfs_get_inode_unlock(t_sb, TMPFS_ROOT_INO);
	ip->i_ops = &tmpfs_iops;
	ip->i_fops = &tmpfs_fops;
	ip->i_sb = sb;
	ip->i_no = t_ip->i_no;
	ip->i_rdev = t_ip->i_rdev;
	ip->i_mode = t_ip->i_mode;
	ip->i_nlink = t_ip->i_nlink;
	ip->i_size = t_ip->i_size;
	ip->i_private = t_ip;

	sblock_add(sb);
	inode_add(ip);

	*mnt_root = dp;
	return 0;
}

static void tmpfs_umount(const char *mount_point)
{
}

static void tmpfs_deinit_inode(struct inode *ip)
{
	struct tmpfs_inode *t_ip = ip->i_private;
	struct tmpfs_super_block *t_sb = ip->i_sb->s_private;

	if (ip->i_nlink > 0) {
		spinlock_acquire(&t_sb->s_lock);
		t_ip->i_nlink = ip->i_nlink;
		t_ip->i_mode = ip->i_mode;
		t_ip->i_size = ip->i_size;
		spinlock_release(&t_sb->s_lock);
	}
}

static int tmpfs_read_inode(struct inode *ip, void *priv)
{
	return 0;
}

static int tmpfs_create(struct dentry *dir_dp, struct dentry *new_dp,
			mode_t mode)
{
	struct inode *dir_ip, *new_ip;
	struct tmpfs_inode *t_new_ip, *t_dir_ip;
	struct tmpfs_super_block *t_sb;
	int err;

	dir_ip = dir_dp->d_inode;
	t_dir_ip = dir_ip->i_private;
	t_sb = dir_ip->i_sb->s_private;

	new_ip = inode_alloc();
	if (!new_ip)
		return -ENOMEM;

	spinlock_acquire(&t_sb->s_lock);

	mode |= S_IFREG;
	t_new_ip = tmpfs_alloc_inode_unlock(t_sb, mode, 0);
	if (!t_new_ip) {
		spinlock_release(&t_sb->s_lock);
		inode_free(new_ip);
		return -ENOMEM;
	}

	err = tmpfs_dir_add_entry_unlock(t_dir_ip, t_new_ip, new_dp->d_name);
	if (err) {
		tmpfs_free_inode_unlock(t_sb, t_new_ip->i_no);
		inode_free(new_ip);
		spinlock_release(&t_sb->s_lock);
		return err;
	}

	new_ip->i_no = t_new_ip->i_no;
	new_ip->i_rdev = t_new_ip->i_rdev;
	new_ip->i_mode = t_new_ip->i_mode;
	new_ip->i_nlink = t_new_ip->i_nlink;
	new_ip->i_size = t_new_ip->i_size;
	new_ip->i_private = t_new_ip;
	new_ip->i_sb = sblock_dup(dir_ip->i_sb);
	new_ip->i_ops = &tmpfs_iops;
	new_ip->i_fops = &tmpfs_fops;

	spinlock_release(&t_sb->s_lock);

	new_dp->d_ops = &tmpfs_dops;
	new_dp->d_inode = new_ip;

	inode_add(new_ip);

	return 0;
}

static int tmpfs_link(struct dentry *old_dp, struct dentry *dir_dp,
		      struct dentry *new_dp)
{
	struct inode *dir_ip, *old_ip;
	struct tmpfs_inode *t_dir_ip, *t_old_ip;
	struct tmpfs_super_block *t_sb;
	int err;

	dir_ip = dir_dp->d_inode;
	t_dir_ip = dir_ip->i_private;
	t_sb = dir_ip->i_sb->s_private;
	old_ip = old_dp->d_inode;
	t_old_ip = old_ip->i_private;

	spinlock_acquire(&t_sb->s_lock);

	err = tmpfs_dir_add_entry_unlock(t_dir_ip, t_old_ip, new_dp->d_name);
	if (err) {
		spinlock_release(&t_sb->s_lock);
		return err;
	}
	++t_old_ip->i_nlink;

	spinlock_release(&t_sb->s_lock);

	new_dp->d_ops = &tmpfs_dops;
	new_dp->d_inode = inode_dup(old_ip);

	sleeplock_acquire(&old_ip->i_lock);
	++old_ip->i_nlink;
	sleeplock_release(&old_ip->i_lock);

	return 0;
}

static int tmpfs_unlink(struct dentry *dir_dp, struct dentry *old_dp)
{
	struct inode *old_ip, *dir_ip;
	struct tmpfs_inode *t_old_ip, *t_dir_ip;
	struct tmpfs_super_block *t_sb;

	old_ip = old_dp->d_inode;
	t_old_ip = old_ip->i_private;
	dir_ip = dir_dp->d_inode;
	t_dir_ip = dir_ip->i_private;
	t_sb = dir_ip->i_sb->s_private;

	spinlock_acquire(&t_sb->s_lock);
	if (t_old_ip->i_nlink > 0)
		--t_old_ip->i_nlink;
	tmpfs_dir_del_entry_unlock(t_sb, t_dir_ip, old_dp->d_name);
	if (t_old_ip->i_nlink == 0)
		tmpfs_free_inode_unlock(t_sb, t_old_ip->i_no);
	spinlock_release(&t_sb->s_lock);

	sleeplock_acquire(&old_ip->i_lock);
	if (old_ip->i_nlink > 0)
		--old_ip->i_nlink;
	sleeplock_release(&old_ip->i_lock);

	return -EOPNOTSUPP;
}

static int tmpfs_mkdir(struct dentry *dir_dp, struct dentry *new_dp,
		       mode_t mode)
{
	struct inode *dir_ip, *new_ip;
	struct tmpfs_inode *t_new_ip, *t_dir_ip;
	struct tmpfs_super_block *t_sb;
	int err;

	dir_ip = dir_dp->d_inode;
	t_dir_ip = dir_ip->i_private;
	t_sb = dir_ip->i_sb->s_private;

	new_ip = inode_alloc();
	if (!new_ip)
		return -ENOMEM;

	spinlock_acquire(&t_sb->s_lock);

	t_new_ip = tmpfs_alloc_dir_inode_unlock(t_sb, mode);
	if (!t_new_ip) {
		spinlock_release(&t_sb->s_lock);
		inode_free(new_ip);
		return -ENOMEM;
	}
	tmpfs_dir_add_entry_unlock(t_new_ip, t_dir_ip, "..");
	t_dir_ip->i_nlink += 1;

	err = tmpfs_dir_add_entry_unlock(t_dir_ip, t_new_ip, new_dp->d_name);
	if (err) {
		t_dir_ip->i_nlink -= 1;
		tmpfs_free_inode_unlock(t_sb, t_new_ip->i_no);
		spinlock_release(&t_sb->s_lock);
		inode_free(new_ip);
		return err;
	}

	new_ip->i_no = t_new_ip->i_no;
	new_ip->i_rdev = t_new_ip->i_rdev;
	new_ip->i_mode = t_new_ip->i_mode;
	new_ip->i_nlink = t_new_ip->i_nlink;
	new_ip->i_size = t_new_ip->i_size;
	new_ip->i_private = t_new_ip;
	new_ip->i_sb = sblock_dup(dir_ip->i_sb);
	new_ip->i_ops = &tmpfs_iops;
	new_ip->i_fops = &tmpfs_fops;

	spinlock_release(&t_sb->s_lock);

	new_dp->d_ops = &tmpfs_dops;
	new_dp->d_inode = new_ip;

	inode_add(new_ip);

	return 0;
}

static int tmpfs_rmdir(struct dentry *dir_dp, struct dentry *old_dp)
{
	struct inode *dir_ip, *old_ip;
	struct tmpfs_inode *t_dir_ip, *t_old_ip;
	struct tmpfs_super_block *t_sb;
	int err;

	dir_ip = dir_dp->d_inode;
	t_dir_ip = dir_ip->i_private;
	old_ip = old_dp->d_inode;
	t_old_ip = old_ip->i_private;
	t_sb = dir_ip->i_sb->s_private;

	spinlock_acquire(&t_sb->s_lock);

	err = tmpfs_dir_del_entry_unlock(t_sb, t_dir_ip, old_dp->d_name);
	if (err) {
		spinlock_release(&t_sb->s_lock);
		return err;
	}

	--t_dir_ip->i_nlink;
	t_old_ip->i_nlink = 0;

	tmpfs_free_inode_unlock(t_sb, t_old_ip->i_no);

	spinlock_release(&t_sb->s_lock);

	sleeplock_acquire(&dir_ip->i_lock);
	--dir_ip->i_nlink;
	sleeplock_release(&dir_ip->i_lock);

	return 0;
}

static int tmpfs_lookup(struct dentry *dir_dp, struct dentry *dp)
{
	struct inode *dir_ip, *ip;
	struct tmpfs_inode *t_dir_ip, *t_ip;
	struct tmpfs_super_block *t_sb;

	dir_ip = dir_dp->d_inode;
	t_dir_ip = dir_ip->i_private;
	t_sb = dir_ip->i_sb->s_private;

	ip = inode_alloc();
	if (!ip)
		return -ENOMEM;

	spinlock_acquire(&t_sb->s_lock);

	t_ip = tmpfs_dir_lookup_unlock(t_sb, t_dir_ip, dp->d_name);
	if (!t_ip) {
		spinlock_release(&t_sb->s_lock);
		inode_free(ip);
		return -ENOENT;
	}

	ip->i_no = t_ip->i_no;
	ip->i_rdev = t_ip->i_rdev;
	ip->i_mode = t_ip->i_mode;
	ip->i_nlink = t_ip->i_nlink;
	ip->i_size = t_ip->i_size;
	ip->i_private = t_ip;
	ip->i_sb = sblock_dup(dir_ip->i_sb);
	ip->i_ops = &tmpfs_iops;
	ip->i_fops = &tmpfs_fops;

	spinlock_release(&t_sb->s_lock);

	dp->d_ops = &tmpfs_dops;
	dp->d_inode = ip;

	inode_add(ip);

	return 0;
}

static int tmpfs_mknod(struct dentry *dir_dp, struct dentry *new_dp,
		       mode_t mode, dev_t dev)
{
	struct inode *dir_ip, *new_ip;
	struct tmpfs_inode *t_new_ip, *t_dir_ip;
	struct tmpfs_super_block *t_sb;
	int err;

	dir_ip = dir_dp->d_inode;
	t_dir_ip = dir_ip->i_private;
	t_sb = dir_ip->i_sb->s_private;

	new_ip = inode_alloc();
	if (!new_ip)
		return -ENOMEM;

	spinlock_acquire(&t_sb->s_lock);

	t_new_ip = tmpfs_alloc_inode_unlock(t_sb, mode, dev);
	if (!t_new_ip) {
		spinlock_release(&t_sb->s_lock);
		inode_free(new_ip);
		return -ENOMEM;
	}

	err = tmpfs_dir_add_entry_unlock(t_dir_ip, t_new_ip, new_dp->d_name);
	if (err) {
		tmpfs_free_inode_unlock(t_sb, t_new_ip->i_no);
		inode_free(new_ip);
		spinlock_release(&t_sb->s_lock);
		return err;
	}

	new_ip->i_no = t_new_ip->i_no;
	new_ip->i_rdev = t_new_ip->i_rdev;
	new_ip->i_mode = t_new_ip->i_mode;
	new_ip->i_nlink = t_new_ip->i_nlink;
	new_ip->i_size = t_new_ip->i_size;
	new_ip->i_private = t_new_ip;
	new_ip->i_sb = sblock_dup(dir_ip->i_sb);
	new_ip->i_ops = &tmpfs_iops;
	new_ip->i_fops = &tmpfs_fops;

	spinlock_release(&t_sb->s_lock);

	new_dp->d_ops = &tmpfs_dops;
	new_dp->d_inode = new_ip;

	inode_add(new_ip);

	return 0;
}

static int tmpfs_rename(struct dentry *old_dir, struct dentry *old_dp,
			struct dentry *new_dir, struct dentry *new_dp)
{
	return 0;
}

static int tmpfs_symlink(struct dentry *dir_dp, struct dentry *dp,
			 const char *target)
{
	return 0;
}

static int tmpfs_readlink(struct dentry *dp, char *buf, size_t len)
{
	return 0;
}

static int tmpfs_read(struct file *fp, void *buf, size_t n, off_t *offset,
		      size_t *rcnt)
{
	struct inode *ip = fp->f_inode;
	struct tmpfs_inode *t_ip = ip->i_private;
	struct tmpfs_super_block *t_sb = ip->i_sb->s_private;
	int ret = 0;

	ip = fp->f_inode;
	t_ip = ip->i_private;
	t_sb = ip->i_sb->s_private;

	spinlock_acquire(&t_sb->s_lock);

	if (S_ISREG(t_ip->i_mode))
		ret = tmpfs_read_file_unlock(t_ip, buf, n, offset, rcnt);
	else if (S_ISDIR(t_ip->i_mode))
		ret = tmpfs_read_dir_unlock(t_ip, buf, n, offset, rcnt);
	else
		ret = -EOPNOTSUPP;

	spinlock_release(&t_sb->s_lock);

	return ret = 0;
}

static int tmpfs_write(struct file *fp, const void *buf, size_t n,
		       off_t *offset, size_t *wcnt)
{
	struct inode *ip = fp->f_inode;
	struct tmpfs_inode *t_ip = ip->i_private;
	struct tmpfs_super_block *t_sb = ip->i_sb->s_private;
	int ret = 0;

	spinlock_acquire(&t_sb->s_lock);

	if (S_ISREG(t_ip->i_mode))
		ret = tmpfs_write_file_unlock(t_ip, buf, n, offset, wcnt);
	else if (S_ISDIR(t_ip->i_mode))
		ret = -EPERM;
	else
		ret = -EOPNOTSUPP;

	spinlock_release(&t_sb->s_lock);

	return ret = 0;
}

static off_t tmpfs_seek(struct file *fp, off_t offset, int whence)
{
	struct inode *ip = fp->f_inode;
	struct tmpfs_inode *t_ip = ip->i_private;
	struct tmpfs_super_block *t_sb = ip->i_sb->s_private;
	int ret = 0;

	if (whence == SEEK_SET) {
		return offset;
	} else if (whence == SEEK_CUR) {
		return fp->f_off + offset;
	} else if (whence == SEEK_END) {
		spinlock_acquire(&t_sb->s_lock);
		ret = tmpfs_seek_unlock(t_ip, fp->f_off + offset);
		spinlock_release(&t_sb->s_lock);
		return ret;
	} else {
		return -EINVAL;
	}
}

static int tmpfs_stat(struct file *fp, struct stat *st)
{
	if (!fp->f_inode)
		return -EBADF;

	memset(st, 0, sizeof(*st));

	sleeplock_acquire(&fp->f_inode->i_lock);
	st->st_dev = fp->f_inode->i_sb->s_dev;
	st->st_ino = fp->f_inode->i_no;
	st->st_mode = fp->f_inode->i_mode;
	st->st_nlink = fp->f_inode->i_nlink;
	st->st_rdev = fp->f_inode->i_rdev;
	st->st_size = fp->f_inode->i_size;
	st->st_blksize = fp->f_inode->i_sb->s_block_size;
	st->st_blocks = fp->f_inode->i_size / st->st_blksize;
	sleeplock_release(&fp->f_inode->i_lock);

	return 0;
}

static int tmpfs_open(struct file *fp, struct dentry *dp, int flags)
{
	int ret = 0;
	struct inode *ip = dp->d_inode;
	mode_t imode = ip->i_mode;
	sleeplock_acquire(&ip->i_lock);
	if (S_ISCHR(imode)) {
		fp->f_inode = inode_dup(ip);
		fp->f_dentry = dentry_dup(dp);
		fp->f_ops = &chrdev_fops;
	} else if (S_ISBLK(imode)) {
		fp->f_inode = inode_dup(ip);
		fp->f_dentry = dentry_dup(dp);
		fp->f_ops = &blkdev_fops;
	} else if (S_ISREG(imode) || S_ISDIR(imode)) {
		fp->f_inode = inode_dup(ip);
		fp->f_dentry = dentry_dup(dp);
		fp->f_ops = &tmpfs_fops;
	} else {
		ret = -EOPNOTSUPP;
	}
	sleeplock_release(&ip->i_lock);
	return ret;
}

static int tmpfs_truncate(struct file *fp, off_t len)
{
	struct inode *ip = fp->f_inode;
	struct tmpfs_inode *t_ip = ip->i_private;
	struct tmpfs_super_block *t_sb = ip->i_sb->s_private;
	int ret = 0;

	spinlock_acquire(&t_sb->s_lock);

	if (S_ISREG(t_ip->i_mode))
		ret = tmpfs_truncate_unlock(t_ip, len);
	else if (S_ISDIR(t_ip->i_mode))
		ret = -EPERM;
	else
		ret = -EOPNOTSUPP;

	spinlock_release(&t_sb->s_lock);

	return ret = 0;
}

static int tmpfs_close(struct file *fp)
{
	return 0;
}

static int tmpfs_dentry_compare(struct dentry *dp, const char *name, size_t len)
{
	return strncmp(dp->d_name, name, len);
}

const struct dentry_operations tmpfs_dops = {
	.compare = tmpfs_dentry_compare,
};

const struct super_operations tmpfs_sops = {
	.deinit_inode = tmpfs_deinit_inode,
	.read_inode = tmpfs_read_inode,
};

const struct inode_operations tmpfs_iops = {
	.create = tmpfs_create,
	.link = tmpfs_link,
	.unlink = tmpfs_unlink,
	.mkdir = tmpfs_mkdir,
	.rmdir = tmpfs_rmdir,
	.lookup = tmpfs_lookup,
	.mknod = tmpfs_mknod,
	.rename = tmpfs_rename,
	.symlink = tmpfs_symlink,
	.readlink = tmpfs_readlink,
};

const struct file_operations tmpfs_fops = {
	.read = tmpfs_read,
	.write = tmpfs_write,
	.seek = tmpfs_seek,
	.stat = tmpfs_stat,
	.open = tmpfs_open,
	.truncate = tmpfs_truncate,
	.close = tmpfs_close,
};

const struct file_system_type tmpfs_fs_type = {
	.name = "tmpfs",
	.deinit_sb = tmpfs_deinit_sb,
	.mount = tmpfs_mount,
	.umount = tmpfs_umount,
};
