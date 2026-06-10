#include <brk/assert.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/pagecache.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

static struct hlist_head inode_hash_table[INODE_HTABLE_SIZE];
static SPINLOCK_DEFINE(inode_hash_lock);
static struct slab_allocator inode_cache;

void fs_inode_cache_init(void)
{
	slab_init(&inode_cache, sizeof(struct fs_inode),
		  alignof(struct fs_inode), "inode_cache");
}

static struct fs_inode *__inode_alloc(struct fs_super_block *sb,
				      unsigned long ino)
{
	struct fs_inode *inode;

	if (sb->ops->alloc_inode) {
		inode = sb->ops->alloc_inode(sb);
	} else {
		inode = slab_alloc(&inode_cache);
		inode->private_data = NULL;
	}

	if (!inode)
		return NULL;

	inode->sb = sb;
	inode->ops = NULL;
	inode->fops = NULL;
	refcnt_init(&inode->count, 1);
	spinlock_init(&inode->lock, "inode.lock");
	sleeplock_init(&inode->rwsem, "inode.rwsem");
	inode->state = 0;
	list_init(&inode->sb_list);
	list_init(&inode->list);
	hlist_node_init(&inode->hash);
	list_init(&inode->dentries);
	inode->ino = ino;
	inode->mode = 0;
	inode->nlink = 0;
	inode->size = 0;
	inode->rdev = 0;
	inode->atime.tv_sec = 0;
	inode->atime.tv_nsec = 0;
	inode->mtime = inode->atime;
	inode->ctime = inode->atime;
	inode->mapping = NULL;

	return inode;
}

static void __inode_free(struct fs_inode *inode)
{
	const struct fs_super_block_ops *op = inode->sb->ops;

	if (op->free_inode)
		op->free_inode(inode);
	else
		slab_free(&inode_cache, inode);
}

static u32 __inode_hash(const struct fs_super_block *sb, unsigned long ino)
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

static struct fs_inode *__inode_lookup(struct hlist_head *bucket,
				       const struct fs_super_block *sb,
				       unsigned long ino)
{
	struct fs_inode *inode;

	ASSERT(spinlock_holding(&inode_hash_lock));

retry:
	/* Look up in hash table */
	hlist_for_each_entry(inode, bucket, hash) {
		if (inode->ino == ino && inode->sb == sb) {
			spinlock_acquire(&inode->lock);

			/* Check if being freed */
			if (inode->state & (I_FREEING | I_WILL_FREE)) {
				spinlock_release(&inode->lock);
				/* Wait for release to complete and retry */
				task_sleep(&inode->state, &inode_hash_lock);
				goto retry;
			}

			spinlock_release(&inode->lock);

			fs_inode_get(inode);
			return inode; /* Cache hit, return directly */
		}
	}

	return NULL;
}

struct fs_inode *fs_inode_get_locked(struct fs_super_block *sb,
				     unsigned long ino)
{
	struct fs_inode *inode, *old;
	struct hlist_head *head = inode_hash_table + __inode_hash(sb, ino);

	spinlock_acquire(&inode_hash_lock);
	inode = __inode_lookup(head, sb, ino);
	spinlock_release(&inode_hash_lock);
	if (inode)
		return inode;

	inode = __inode_alloc(sb, ino);
	if (!inode)
		return NULL;

	spinlock_acquire(&inode_hash_lock);
	old = __inode_lookup(head, sb, ino);
	if (!old) {
		/*
		 * Currently, the inode has not been added to the hash table,
		 * so it will not be found by other processes. We can set I_NEW
		 * directly without locking lock.
		 */
		inode->state |= I_NEW;

		hlist_add_head(&inode->hash, head);
		spinlock_release(&inode_hash_lock);

		spinlock_acquire(&sb->inodes_lock);
		list_add(&inode->sb_list, &sb->inodes);
		spinlock_release(&sb->inodes_lock);

		return inode;
	}
	spinlock_release(&inode_hash_lock);

	__inode_free(inode);

	inode = old;

	while (1) {
		spinlock_acquire(&inode->lock);
		if (!(inode->state & I_NEW)) {
			spinlock_release(&inode->lock);
			break;
		}
		task_sleep(&inode->state, &inode->lock);
	}

	return inode;
}

void fs_inode_unlock_new(struct fs_inode *inode)
{
	spinlock_acquire(&inode->lock);
	inode->state &= ~I_NEW;
	spinlock_release(&inode->lock);
	task_wake_all(&inode->state);
}

struct fs_inode *fs_inode_get(struct fs_inode *inode)
{
	refcnt_inc(&inode->count);
	return inode;
}

void fs_inode_put(struct fs_inode *inode)
{
	const struct fs_super_block_ops *s_op;

	if (refcnt_dec_fetch(&inode->count) > 0)
		return;

	spinlock_acquire(&inode->lock);
	inode->state |= I_FREEING;
	spinlock_release(&inode->lock);

	s_op = inode->sb->ops;
	if (s_op->evict_inode)
		s_op->evict_inode(inode);

	/*
	 * Drop the page cache after the filesystem has had a chance to flush
	 * any dirty data in ->evict_inode(). Pages still alive at this point
	 * are dropped without writeback.
	 */
	if (inode->mapping) {
		page_cache_destroy(inode->mapping);
		inode->mapping = NULL;
	}

	spinlock_acquire(&inode_hash_lock);
	hlist_del_init(&inode->hash);
	spinlock_release(&inode_hash_lock);

	spinlock_acquire(&inode->sb->inodes_lock);
	list_del(&inode->sb_list);
	if (inode->state & I_DIRTY)
		list_del(&inode->list);
	spinlock_release(&inode->sb->inodes_lock);

	__inode_free(inode);
}

int fs_inode_attach_page_cache(struct fs_inode *inode,
			       const struct page_cache_ops *ops)
{
	struct page_cache *m;

	if (inode->mapping)
		return 0;
	m = page_cache_create(inode, ops);
	if (!m)
		return -ENOMEM;
	inode->mapping = m;
	return 0;
}

void fs_inode_mark_dirty(struct fs_inode *inode)
{
	struct fs_super_block *sb = inode->sb;
	bool need_link;

	spinlock_acquire(&inode->lock);
	need_link = (inode->state & I_DIRTY) == 0;
	inode->state |= I_DIRTY;
	spinlock_release(&inode->lock);

	if (!need_link)
		return;

	spinlock_acquire(&sb->inodes_lock);
	list_add(&inode->list, &sb->dirty_inodes);
	spinlock_release(&sb->inodes_lock);
}

void fs_inode_clear(struct fs_inode *inode)
{
	spinlock_acquire(&inode->lock);
	inode->state |= I_CLEAR;
	spinlock_release(&inode->lock);
}
