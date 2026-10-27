#include <brk/assert.h>
#include <brk/fs.h>
#include <brk/initcall.h>
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
#include <uapi/errno.h>

static struct hlist_head inode_hash_table[INODE_HTABLE_SIZE];
static SPINLOCK_DEFINE(inode_hash_lock);
static struct slab_allocator inode_cache;

void fs_inode_cache_init(void)
{
	slab_init(&inode_cache, sizeof(struct fs_inode),
		  alignof(struct fs_inode), "inode_cache");
}
core_initcall(fs_inode_cache_init);

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
	for (size_t i = 0; i < sizeof(void *); ++i) {
		h ^= k[i];
		h *= 0x01000193;
	}
	k = (u8 *)&ino;
	for (size_t i = 0; i < sizeof(ino); ++i) {
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

	ASSERT(spin_is_locked_by_me(&inode_hash_lock));

retry:
	/* Look up in hash table */
	hlist_for_each_entry(inode, bucket, hash) {
		if (inode->ino == ino && inode->sb == sb) {
			irqstate_t flags = spin_lock_irqsave(&inode->lock);

			/*
			 * Check if being freed, and take the reference while
			 * holding inode->lock.  fs_inode_put() drops the last
			 * reference before it sets I_FREEING, so a plain
			 * refcnt_inc() could resurrect an inode that is about
			 * to be freed.  inc_unless_zero() closes that window;
			 * if it fails, wait for the eviction to finish and
			 * retry the hash lookup.
			 */
			if ((inode->state & (I_FREEING | I_WILL_FREE | I_BAD)) ||
			    !refcnt_inc_unless_zero(&inode->count)) {
				spin_unlock_irqrestore(&inode->lock, flags);
				task_sleep(&inode->state, &inode_hash_lock);
				goto retry;
			}

			spin_unlock_irqrestore(&inode->lock, flags);
			return inode; /* Cache hit, return directly */
		}
	}

	return NULL;
}

/*
 * Wait for an inode that another CPU is initializing (I_NEW).  The caller
 * already owns a reference, so the inode cannot disappear while we sleep.
 *
 * Return true when the inode is ready to use, false if initialization
 * failed (I_BAD) or the inode started dying.  In the false case the caller
 * must drop its reference and retry the hash lookup.
 */
static bool inode_wait_new(struct fs_inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	while (inode->state & I_NEW)
		task_sleep(&inode->state, &inode->lock);

	bool ready = !(inode->state & (I_BAD | I_FREEING | I_WILL_FREE));
	spin_unlock_irqrestore(&inode->lock, flags);
	return ready;
}

struct fs_inode *fs_inode_get_locked(struct fs_super_block *sb,
				     unsigned long ino)
{
	struct fs_inode *inode, *old;
	struct hlist_head *head = inode_hash_table + __inode_hash(sb, ino);
	irqstate_t flags;

retry:
	flags = spin_lock_irqsave(&inode_hash_lock);
	inode = __inode_lookup(head, sb, ino);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	if (inode) {
		if (!inode_wait_new(inode)) {
			fs_inode_put(inode);
			goto retry;
		}
		return inode;
	}

	inode = __inode_alloc(sb, ino);
	if (!inode)
		return NULL;

	flags = spin_lock_irqsave(&inode_hash_lock);
	old = __inode_lookup(head, sb, ino);
	if (!old) {
		/*
		 * Currently, the inode has not been added to the hash table,
		 * so it will not be found by other processes. We can set I_NEW
		 * directly without locking lock.
		 */
		inode->state |= I_NEW;

		hlist_add_head(&inode->hash, head);
		spin_unlock_irqrestore(&inode_hash_lock, flags);

		spin_lock(&sb->inodes_lock);
		list_add(&inode->sb_list, &sb->inodes);
		spin_unlock(&sb->inodes_lock);

		return inode;
	}
	spin_unlock_irqrestore(&inode_hash_lock, flags);

	__inode_free(inode);

	inode = old;
	if (!inode_wait_new(inode)) {
		fs_inode_put(inode);
		goto retry;
	}

	return inode;
}

void fs_inode_unlock_new(struct fs_inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	inode->state &= ~I_NEW;
	spin_unlock_irqrestore(&inode->lock, flags);
	task_wake_all(&inode->state);
}

/*
 * Mark a never-published I_NEW inode as unusable.  Waiters are woken and
 * will drop their temporary references; the inode is then freed by the
 * normal fs_inode_put() path without running ->evict_inode() on partially
 * initialized state.
 */
void fs_inode_fail_new(struct fs_inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	inode->state |= I_BAD;
	inode->state &= ~I_NEW;
	spin_unlock_irqrestore(&inode->lock, flags);
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
	bool bad;

	if (refcnt_dec_fetch(&inode->count) > 0)
		return;

	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	bad = (inode->state & I_BAD) != 0;
	inode->state |= I_FREEING;
	spin_unlock_irqrestore(&inode->lock, flags);

	s_op = inode->sb->ops;
	/*
	 * An I_BAD inode failed during I_NEW initialization.  Its filesystem
	 * private data may be incomplete, so do not call ->evict_inode();
	 * ->free_inode() still releases the allocation itself.
	 */
	if (!bad && s_op->evict_inode)
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

	irqstate_t hflags = spin_lock_irqsave(&inode_hash_lock);

	hlist_del_init(&inode->hash);
	spin_unlock_irqrestore(&inode_hash_lock, hflags);

	spin_lock(&inode->sb->inodes_lock);
	list_del(&inode->sb_list);
	if (inode->state & I_DIRTY)
		list_del(&inode->list);
	spin_unlock(&inode->sb->inodes_lock);

	/*
	 * Wake every lookup that found this inode while it was dying.  The
	 * inode has already been removed from the hash table, so the waiters
	 * will retry from scratch and either allocate a fresh inode or find
	 * one created in the meantime.
	 */
	task_wake_all(&inode->state);

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

	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	need_link = (inode->state & I_DIRTY) == 0;
	inode->state |= I_DIRTY;
	spin_unlock_irqrestore(&inode->lock, flags);

	if (!need_link)
		return;

	spin_lock(&sb->inodes_lock);
	list_add(&inode->list, &sb->dirty_inodes);
	spin_unlock(&sb->inodes_lock);
}

void fs_inode_clear(struct fs_inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	inode->state |= I_CLEAR;
	spin_unlock_irqrestore(&inode->lock, flags);
}
