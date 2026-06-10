#include <brk/fs/fs.h>
#include <brk/kernel/refcnt.h>
#include <brk/lib/list.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/pagecache.h>

static LIST_DEFINE(super_blocks);
static SPINLOCK_DEFINE(sb_lock);

struct fs_super_block *fs_super_block_alloc(struct fs_driver *driver)
{
	struct fs_super_block *sb;

	sb = kzalloc(sizeof(*sb));
	if (!sb)
		return NULL;

	list_init(&sb->list);
	sb->driver = driver;
	list_init(&sb->instance);
	refcnt_init(&sb->count, 1);

	spinlock_init(&sb->inodes_lock, "super_block.inodes_lock");
	list_init(&sb->inodes);
	list_init(&sb->dirty_inodes);
	spinlock_init(&sb->mnt_states_lock, "super_block.mnt_states_lock");
	list_init(&sb->mnt_states);

	spinlock_acquire(&sb_lock);
	list_add_tail(&sb->list, &super_blocks);
	spinlock_release(&sb_lock);

	return sb;
}

void fs_super_block_free(struct fs_super_block *sb)
{
	spinlock_acquire(&sb_lock);
	list_del_init(&sb->list);
	spinlock_release(&sb_lock);

	kfree(sb);
}

void fs_super_block_get(struct fs_super_block *sb)
{
	refcnt_inc(&sb->count);
}

void fs_super_block_put(struct fs_super_block *sb)
{
	if (refcnt_dec_fetch(&sb->count) > 0)
		return;

	if (sb->ops->put_super)
		sb->ops->put_super(sb);
	fs_super_block_free(sb);
}

/*
 * Write a single dirty inode back to its backing store: data pages first
 * (so the on-disk size/block pointers the metadata refers to are already
 * present), then the inode metadata itself.
 *
 * The inode rwsem is held to exclude concurrent file I/O and truncation,
 * matching the locking used by the per-file ->fsync path.
 */
static int writeback_single_inode(struct fs_inode *inode)
{
	const struct fs_super_block_ops *ops = inode->sb->ops;
	int ret = 0;
	int err;

	sleeplock_acquire(&inode->rwsem);
	if (inode->mapping) {
		err = page_cache_flush(inode->mapping);
		if (err)
			ret = err;
	}
	if (ops->write_inode) {
		err = ops->write_inode(inode, 1);
		if (err && !ret)
			ret = err;
	}
	sleeplock_release(&inode->rwsem);
	return ret;
}

int sync_filesystem(struct fs_super_block *sb)
{
	LIST_DEFINE(work);
	struct fs_inode *inode, *n;
	int ret = 0;

	if (!sb)
		return 0;

	/*
	 * Snapshot the dirty inode list: pin every inode that is not being
	 * torn down and move it onto a private work list. Inodes keep their
	 * I_DIRTY marker while parked there, so a concurrent dirtier (whose
	 * fs_inode_mark_dirty() only links when I_DIRTY is clear) will not try
	 * to re-link an inode that is already on the work list.
	 */
	spinlock_acquire(&sb->inodes_lock);
	list_for_each_entry_safe(inode, n, &sb->dirty_inodes, list) {
		spinlock_acquire(&inode->lock);
		/*
		 * Skip inodes that are on their way out, and pin the rest with
		 * refcnt_inc_unless_zero() rather than fs_inode_get(): if
		 * fs_inode_put() has already dropped the last reference (but not
		 * yet set I_FREEING), the count is zero and we must not
		 * resurrect the inode. Doing both under inode->lock makes the
		 * I_FREEING check and the pin atomic against that teardown.
		 */
		if ((inode->state & (I_FREEING | I_WILL_FREE)) ||
		    !refcnt_inc_unless_zero(&inode->count)) {
			spinlock_release(&inode->lock);
			continue;
		}
		spinlock_release(&inode->lock);
		list_move_tail(&inode->list, &work);
	}
	spinlock_release(&sb->inodes_lock);

	while (!list_empty(&work)) {
		int err;

		/*
		 * Clear I_DIRTY before the IO (the same clear-before-write
		 * ordering the page cache uses): a writer that re-dirties the
		 * inode during writeback re-links it on sb->dirty_inodes and it
		 * is caught by a later sync.
		 */
		spinlock_acquire(&sb->inodes_lock);
		inode = list_first_entry(&work, struct fs_inode, list);
		list_del_init(&inode->list);
		spinlock_acquire(&inode->lock);
		inode->state &= ~I_DIRTY;
		spinlock_release(&inode->lock);
		spinlock_release(&sb->inodes_lock);

		err = writeback_single_inode(inode);
		if (err && !ret)
			ret = err;
		fs_inode_put(inode);
	}

	return ret;
}

int sync_all_filesystems(void)
{
	struct fs_super_block *sb, *next;
	int ret = 0;

	/*
	 * Walk the global superblock list with a pinned cursor: each sb keeps
	 * a reference across the (blocking) sync so it cannot be freed, and a
	 * pinned sb stays linked, so we can safely read its successor after
	 * re-taking the lock.
	 */
	spinlock_acquire(&sb_lock);
	sb = list_empty(&super_blocks) ?
		     NULL :
		     list_first_entry(&super_blocks, struct fs_super_block,
				      list);
	if (sb)
		fs_super_block_get(sb);
	spinlock_release(&sb_lock);

	while (sb) {
		int err = sync_filesystem(sb);
		if (err && !ret)
			ret = err;

		spinlock_acquire(&sb_lock);
		next = list_is_last(&sb->list, &super_blocks) ?
			       NULL :
			       list_next_entry(sb, list);
		if (next)
			fs_super_block_get(next);
		spinlock_release(&sb_lock);

		fs_super_block_put(sb);
		sb = next;
	}

	return ret;
}
