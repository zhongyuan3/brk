#include <brk/assert.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/pagecache.h>
#include <brk/process.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

static struct hlist_head inode_hash_table[INODE_HTABLE_SIZE];
static SPINLOCK_DEFINE(inode_hash_lock);
static struct kobj_pool inode_cache;

void inode_cache_init(void)
{
	kobj_pool_init(&inode_cache, sizeof(struct fs_inode),
		       alignof(struct fs_inode), "inode_cache");
}

static struct fs_inode *alloc_inode(struct fs_state *sb, unsigned long ino)
{
	struct fs_inode *inode;

	if (sb->s_op->alloc_inode) {
		inode = sb->s_op->alloc_inode(sb);
	} else {
		inode = kobj_pool_alloc(&inode_cache);
		inode->i_private = NULL;
	}

	if (!inode)
		return NULL;

	inode->i_sb = sb;
	inode->i_op = NULL;
	inode->i_fop = NULL;
	refcnt_init(&inode->i_count, 1);
	spinlock_init(&inode->i_lock, "inode.i_lock");
	sleeplock_init(&inode->i_rwsem, "inode.i_rwsem");
	inode->i_state = 0;
	list_init(&inode->i_sb_list);
	list_init(&inode->i_list);
	hlist_node_init(&inode->i_hash);
	list_init(&inode->i_dentry);
	inode->i_ino = ino;
	inode->i_mode = 0;
	inode->i_nlink = 0;
	inode->i_size = 0;
	inode->i_rdev = 0;
	inode->i_atime.tv_sec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime = inode->i_atime;
	inode->i_ctime = inode->i_atime;
	inode->i_mapping = NULL;

	return inode;
}

static void free_inode(struct fs_inode *inode)
{
	const struct fs_state_ops *op = inode->i_sb->s_op;

	if (op->free_inode)
		op->free_inode(inode);
	else
		kobj_pool_free(&inode_cache, inode);
}

static u32 hash(const struct fs_state *sb, unsigned long ino)
{
	const u8 *k = (u8 *)&sb;
	u32 h = 0x811c9dc5;
	for (usize_t i = 0; i < sizeof(void *); ++i) {
		h ^= k[i];
		h *= 0x01000193;
	}
	k = (u8 *)&ino;
	for (usize_t i = 0; i < sizeof(ino); ++i) {
		h ^= k[i];
		h *= 0x01000193;
	}
	return h & (INODE_HTABLE_SIZE - 1);
}

static struct fs_inode *lookup_inode(struct hlist_head *bucket,
				     const struct fs_state *sb,
				     unsigned long ino)
{
	struct fs_inode *inode;

	ASSERT(spinlock_holding(&inode_hash_lock));

retry:
	/* Look up in hash table */
	hlist_for_each_entry(inode, bucket, i_hash) {
		if (inode->i_ino == ino && inode->i_sb == sb) {
			spinlock_acquire(&inode->i_lock);

			/* Check if being freed */
			if (inode->i_state & (I_FREEING | I_WILL_FREE)) {
				spinlock_release(&inode->i_lock);
				/* Wait for release to complete and retry */
				proc_sleep(&inode->i_state, &inode_hash_lock);
				goto retry;
			}

			spinlock_release(&inode->i_lock);

			inode_dup(inode);
			return inode; /* Cache hit, return directly */
		}
	}

	return NULL;
}

/**
 * inode_get_locked - Look up inode from cache, allocate new if not exists
 * @sb:  Super block
 * @ino: inode number
 *
 * The returned inode is in one of two states:
 *   A. Exists (cache hit): I_NEW not set, reference count increased, ready to use
 *   B. Newly allocated: I_NEW set, caller must read data from disk and call inode_unlock_new()
 *
 * Guarantee: For the same (sb, ino), only one caller of concurrent inode_get_locked()
 *       will receive an inode in I_NEW state (other callers will wait for inode_unlock_new()).
 */
struct fs_inode *inode_get_locked(struct fs_state *sb, unsigned long ino)
{
	struct fs_inode *inode, *old;
	struct hlist_head *head = inode_hash_table + hash(sb, ino);

	spinlock_acquire(&inode_hash_lock);
	inode = lookup_inode(head, sb, ino);
	spinlock_release(&inode_hash_lock);
	if (inode)
		return inode;

	inode = alloc_inode(sb, ino);
	if (!inode)
		return NULL;

	spinlock_acquire(&inode_hash_lock);
	old = lookup_inode(head, sb, ino);
	if (!old) {
		/*
		 * Currently, the inode has not been added to the hash table,
		 * so it will not be found by other processes. We can set I_NEW
		 * directly without locking i_lock.
		 */
		inode->i_state |= I_NEW;

		hlist_add_head(&inode->i_hash, head);
		spinlock_release(&inode_hash_lock);

		spinlock_acquire(&sb->s_inode_lock);
		list_add(&inode->i_sb_list, &sb->s_inodes);
		spinlock_release(&sb->s_inode_lock);

		return inode;
	}
	spinlock_release(&inode_hash_lock);

	free_inode(inode);

	inode = old;

	while (1) {
		spinlock_acquire(&inode->i_lock);
		if (!(inode->i_state & I_NEW)) {
			spinlock_release(&inode->i_lock);
			break;
		}
		proc_sleep(&inode->i_state, &inode->i_lock);
	}

	return inode;
}

void inode_unlock_new(struct fs_inode *inode)
{
	spinlock_acquire(&inode->i_lock);
	inode->i_state &= ~I_NEW;
	spinlock_release(&inode->i_lock);
	proc_wake_up(&inode->i_state);
}

struct fs_inode *inode_dup(struct fs_inode *inode)
{
	refcnt_inc(&inode->i_count);
	return inode;
}

void inode_put(struct fs_inode *inode)
{
	const struct fs_state_ops *s_op;

	if (refcnt_dec_fetch(&inode->i_count) > 0)
		return;

	spinlock_acquire(&inode->i_lock);
	inode->i_state |= I_FREEING;
	spinlock_release(&inode->i_lock);

	s_op = inode->i_sb->s_op;
	if (s_op->evict_inode)
		s_op->evict_inode(inode);

	/*
	 * Drop the page cache after the filesystem has had a chance to flush
	 * any dirty data in ->evict_inode(). Pages still alive at this point
	 * are dropped without writeback.
	 */
	if (inode->i_mapping) {
		address_space_free(inode->i_mapping);
		inode->i_mapping = NULL;
	}

	spinlock_acquire(&inode_hash_lock);
	hlist_del_init(&inode->i_hash);
	spinlock_release(&inode_hash_lock);

	spinlock_acquire(&inode->i_sb->s_inode_lock);
	list_del(&inode->i_sb_list);
	if (inode->i_state & I_DIRTY)
		list_del(&inode->i_list);
	spinlock_release(&inode->i_sb->s_inode_lock);

	free_inode(inode);
}

int inode_attach_pagecache(struct fs_inode *inode,
			   const struct page_cache_ops *a_ops)
{
	struct page_cache *m;

	if (inode->i_mapping)
		return 0;
	m = address_space_alloc(inode, a_ops);
	if (!m)
		return -ENOMEM;
	inode->i_mapping = m;
	return 0;
}

void inode_mark_dirty(struct fs_inode *inode)
{
	struct fs_state *sb = inode->i_sb;
	bool need_link;

	spinlock_acquire(&inode->i_lock);
	need_link = (inode->i_state & I_DIRTY) == 0;
	inode->i_state |= I_DIRTY;
	spinlock_release(&inode->i_lock);

	if (!need_link)
		return;

	spinlock_acquire(&sb->s_inode_lock);
	list_add(&inode->i_list, &sb->s_dirty);
	spinlock_release(&sb->s_inode_lock);
}

void inode_clear(struct fs_inode *inode)
{
	spinlock_acquire(&inode->i_lock);
	inode->i_state |= I_CLEAR;
	spinlock_release(&inode->i_lock);
}
