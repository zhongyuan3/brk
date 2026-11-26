#include <brk/assert.h>
#include <brk/fs.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/pagecache.h>
#include <brk/refcnt.h>
#include <brk/rwsem.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <stdalign.h>
#include <uapi/errno.h>

static struct hlist_head inode_hash_table[INODE_HTABLE_SIZE];
static SPINLOCK_DEFINE(inode_hash_lock);
static struct slab_allocator inode_cache;

void inode_cache_init(void)
{
	slab_init(&inode_cache, sizeof(struct inode), alignof(struct inode),
		  "inode_cache");
}

static struct inode *__inode_alloc(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;

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
	rwsem_init(&inode->rwsem, "inode.rwsem");
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
	inode->atime = 0;
	inode->mtime = 0;
	inode->ctime = 0;
	inode->mapping = NULL;

	return inode;
}

static void __inode_free(struct inode *inode)
{
	const struct super_block_ops *op = inode->sb->ops;

	if (op->free_inode)
		op->free_inode(inode);
	else
		slab_free(&inode_cache, inode);
}

static u32 __inode_hash(const struct super_block *sb, unsigned long ino)
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

static struct inode *__inode_lookup(struct hlist_head *bucket,
				    const struct super_block *sb,
				    unsigned long ino)
{
	struct inode *inode;

	kassert(spin_is_locked_by_me(&inode_hash_lock));

retry:
	/* Look up in hash table */
	hlist_for_each_entry(inode, bucket, hash) {
		if (inode->ino == ino && inode->sb == sb) {
			irqstate_t flags = spin_lock_irqsave(&inode->lock);

			/*
			 * Check if being freed, and take the reference while
			 * holding inode->lock.  inode_put() drops the last
			 * reference before it sets I_FREEING, so a plain
			 * refcnt_inc() could resurrect an inode that is about
			 * to be freed.  inc_unless_zero() closes that window;
			 * if it fails, wait for the eviction to finish and
			 * retry the hash lookup.
			 */
			if ((inode->state &
			     (I_FREEING | I_WILL_FREE | I_BAD)) ||
			    !refcnt_inc_unless_zero(&inode->count)) {
				spin_unlock_irqrestore(&inode->lock, flags);
				thread_sleep_on(&inode->state,
						&inode_hash_lock);
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
static bool inode_wait_new(struct inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	while (inode->state & I_NEW)
		thread_sleep_on(&inode->state, &inode->lock);

	bool ready = !(inode->state & (I_BAD | I_FREEING | I_WILL_FREE));
	spin_unlock_irqrestore(&inode->lock, flags);
	return ready;
}

struct inode *inode_get_locked(struct super_block *sb, unsigned long ino)
{
	struct inode *inode, *old;
	struct hlist_head *head = inode_hash_table + __inode_hash(sb, ino);
	irqstate_t flags;

retry:
	flags = spin_lock_irqsave(&inode_hash_lock);
	inode = __inode_lookup(head, sb, ino);
	spin_unlock_irqrestore(&inode_hash_lock, flags);
	if (inode) {
		if (!inode_wait_new(inode)) {
			inode_put(inode);
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
		inode_put(inode);
		goto retry;
	}

	return inode;
}

void inode_unlock_new(struct inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	inode->state &= ~I_NEW;
	spin_unlock_irqrestore(&inode->lock, flags);
	thread_wake_all(&inode->state);
}

/*
 * Mark a never-published I_NEW inode as unusable.  Waiters are woken and
 * will drop their temporary references; the inode is then freed by the
 * normal inode_put() path without running ->evict_inode() on partially
 * initialized state.
 */
void inode_fail_new(struct inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	inode->state |= I_BAD;
	inode->state &= ~I_NEW;
	spin_unlock_irqrestore(&inode->lock, flags);
	thread_wake_all(&inode->state);
}

struct inode *inode_get(struct inode *inode)
{
	refcnt_inc(&inode->count);
	return inode;
}

void inode_put(struct inode *inode)
{
	const struct super_block_ops *s_op;
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
	thread_wake_all(&inode->state);

	__inode_free(inode);
}

int inode_attach_page_cache(struct inode *inode,
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

void inode_mark_dirty(struct inode *inode)
{
	struct super_block *sb = inode->sb;
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

void inode_clear(struct inode *inode)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	inode->state |= I_CLEAR;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_times_set_all_now(struct inode *inode)
{
	ktime_t now = ktime_realtime();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->atime = now;
	inode->mtime = now;
	inode->ctime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_times_set(struct inode *inode, const ktime_t *atime,
		     const ktime_t *mtime)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	if (atime)
		inode->atime = *atime;
	if (mtime)
		inode->mtime = *mtime;

	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_mtime(struct inode *inode)
{
	ktime_t now = ktime_realtime();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->mtime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_ctime(struct inode *inode)
{
	ktime_t now = ktime_realtime();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->ctime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_touch_mtime_ctime(struct inode *inode)
{
	ktime_t now = ktime_realtime();
	irqstate_t flags;

	flags = spin_lock_irqsave(&inode->lock);
	inode->mtime = now;
	inode->ctime = now;
	spin_unlock_irqrestore(&inode->lock, flags);
}

void inode_times_to_kstat(struct inode *inode, struct kstat *ks)
{
	irqstate_t flags = spin_lock_irqsave(&inode->lock);

	ks->atime = inode->atime;
	ks->mtime = inode->mtime;
	ks->ctime = inode->ctime;

	spin_unlock_irqrestore(&inode->lock, flags);
}
