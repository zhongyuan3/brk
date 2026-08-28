#include <brk/base/assert.h>
#include <brk/base/error.h>
#include <brk/base/kernel.h>
#include <brk/base/list.h>
#include <brk/base/types.h>
#include <brk/fs/fs.h>
#include <brk/init/initcall.h>
#include <brk/lib/refcnt.h>
#include <brk/lib/string.h>
#include <brk/lock/sleeplock.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/pagecache.h>
#include <brk/mm/pgalloc.h>
#include <brk/mm/slab.h>
#include <brk/printk/printk.h>
#include <brk/time/ktime.h>
#include <uapi/brk/errno.h>

static struct slab_allocator cached_page_cache;

static LIST_DEFINE(pagecache_lru);
static SPINLOCK_DEFINE(pagecache_lru_lock);
static unsigned long pagecache_nrpages;

static void pagecache_lru_add(struct cached_page *cp)
{
	spinlock_acquire(&pagecache_lru_lock);
	if (list_empty(&cp->lru_list))
		list_add_tail(&cp->lru_list, &pagecache_lru);
	spinlock_release(&pagecache_lru_lock);
}

static void pagecache_lru_del(struct cached_page *cp)
{
	spinlock_acquire(&pagecache_lru_lock);
	if (!list_empty(&cp->lru_list))
		list_del_init(&cp->lru_list);
	spinlock_release(&pagecache_lru_lock);
}

static void pagecache_lru_touch(struct cached_page *cp)
{
	spinlock_acquire(&pagecache_lru_lock);
	if (!list_empty(&cp->lru_list)) {
		list_del(&cp->lru_list);
		list_add_tail(&cp->lru_list, &pagecache_lru);
	}
	spinlock_release(&pagecache_lru_lock);
}

void page_cache_init(void)
{
	list_init(&pagecache_lru);
	slab_init(&cached_page_cache, sizeof(struct cached_page),
		  alignof(struct cached_page), "cached_page");
}
core_initcall(page_cache_init);

static unsigned int mapping_hash(pgoff_t index)
{
	uint64_t h = (uint64_t)index;
	h *= 0x9E3779B97F4A7C15ULL;
	h ^= h >> 32;
	return (unsigned int)(h & (PAGE_CACHE_HSIZE - 1));
}

struct page_cache *page_cache_create(void *host,
				     const struct page_cache_ops *ops)
{
	struct page_cache *m;

	m = kzalloc(sizeof(*m));
	if (!m)
		return NULL;

	m->host = host;
	m->ops = ops;
	spinlock_init(&m->lock, "page_cache.lock");
	for (size_t i = 0; i < PAGE_CACHE_HSIZE; ++i)
		hlist_head_init(&m->pages[i]);
	list_init(&m->dirty_pages);
	m->nrpages = 0;
	return m;
}

/* Caller must hold @m->lock. */
static void __detach_locked(struct page_cache *m, struct cached_page *cp)
{
	hlist_del_init(&cp->ht_node);
	if (cp->flags & PCP_DIRTY) {
		list_del_init(&cp->dirty_list);
		cp->flags &= ~PCP_DIRTY;
	}
	pagecache_lru_del(cp);
	m->nrpages--;
	pagecache_nrpages--;
}

/* Drop the implicit cache reference for a page that has just been removed
 * from the hash. May free the page. */
static void detach_and_put(struct cached_page *cp)
{
	cached_page_put(cp);
}

void page_cache_destroy(struct page_cache *m)
{
	struct cached_page *cp;
	struct hlist_node *next;

	if (!m)
		return;

	spinlock_acquire(&m->lock);
	for (size_t i = 0; i < PAGE_CACHE_HSIZE; ++i) {
		hlist_for_each_entry_safe(cp, next, &m->pages[i], ht_node) {
			__detach_locked(m, cp);
			spinlock_release(&m->lock);
			detach_and_put(cp);
			spinlock_acquire(&m->lock);
		}
	}
	spinlock_release(&m->lock);

	kfree(m);
}

static struct cached_page *cached_page_alloc(struct page_cache *m,
					     pgoff_t index)
{
	struct cached_page *cp;
	struct page *pg;

	cp = slab_alloc(&cached_page_cache);
	if (!cp)
		return NULL;
	pg = page_zalloc(0);
	if (!pg) {
		slab_free(&cached_page_cache, cp);
		return NULL;
	}

	/* Refcount starts at 1 for the cache's implicit reference. The user
	 * reference will be added by the insertion path below. */
	refcnt_init(&cp->refcnt, 1);
	cp->page = pg;
	cp->mapping = m;
	cp->index = index;
	cp->flags = 0;
	spinlock_init(&cp->lock, "cached_page.lock");
	sleeplock_init(&cp->io_lock, "cached_page.io_lock");
	hlist_node_init(&cp->ht_node);
	list_init(&cp->dirty_list);
	list_init(&cp->lru_list);
	return cp;
}

static void cached_page_destroy(struct cached_page *cp)
{
	page_free(cp->page, 0);
	slab_free(&cached_page_cache, cp);
}

static struct cached_page *__lookup_locked(struct page_cache *m, pgoff_t index)
{
	struct hlist_head *bucket = &m->pages[mapping_hash(index)];
	struct cached_page *cp;

	hlist_for_each_entry(cp, bucket, ht_node) {
		if (cp->index == index)
			return cp;
	}
	return NULL;
}

static struct cached_page *find_page(struct page_cache *m, pgoff_t index)
{
	struct cached_page *cp;

	spinlock_acquire(&m->lock);
	cp = __lookup_locked(m, index);
	if (cp)
		refcnt_inc(&cp->refcnt);
	spinlock_release(&m->lock);
	if (cp)
		pagecache_lru_touch(cp);
	return cp;
}

static struct cached_page *find_or_create_page(struct page_cache *m,
					       pgoff_t index)
{
	struct cached_page *cp, *existing;

	cp = find_page(m, index);
	if (cp)
		return cp;

	cp = cached_page_alloc(m, index);
	if (!cp)
		return NULL;

	spinlock_acquire(&m->lock);
	existing = __lookup_locked(m, index);
	if (existing) {
		refcnt_inc(&existing->refcnt);
		spinlock_release(&m->lock);
		cached_page_destroy(cp);
		return existing;
	}
	hlist_add_head(&cp->ht_node, &m->pages[mapping_hash(index)]);
	m->nrpages++;
	pagecache_nrpages++;
	refcnt_inc(&cp->refcnt); /* user reference returned to caller */
	spinlock_release(&m->lock);

	pagecache_lru_add(cp);
	return cp;
}

void cached_page_get(struct cached_page *cp)
{
	refcnt_inc(&cp->refcnt);
}

void cached_page_put(struct cached_page *cp)
{
	if (!cp)
		return;
	if (refcnt_dec_fetch(&cp->refcnt) > 0)
		return;

	/*
	 * refcount reached zero. By design the cache reference is dropped
	 * only after the page has been removed from the hash, so by this
	 * point the page is unreachable from the cache.
	 */
	ASSERT(hlist_unhashed(&cp->ht_node));
	cached_page_destroy(cp);
}

void cached_page_lock(struct cached_page *cp)
{
	sleeplock_acquire(&cp->io_lock);
}

void cached_page_unlock(struct cached_page *cp)
{
	sleeplock_release(&cp->io_lock);
}

bool cached_page_uptodate(struct cached_page *cp)
{
	bool ret;
	spinlock_acquire(&cp->lock);
	ret = (cp->flags & PCP_UPTODATE) != 0;
	spinlock_release(&cp->lock);
	return ret;
}

bool cached_page_dirty(struct cached_page *cp)
{
	bool ret;
	spinlock_acquire(&cp->lock);
	ret = (cp->flags & PCP_DIRTY) != 0;
	spinlock_release(&cp->lock);
	return ret;
}

bool cached_page_writeback(struct cached_page *cp)
{
	bool ret;
	spinlock_acquire(&cp->lock);
	ret = (cp->flags & PCP_WRITEBACK) != 0;
	spinlock_release(&cp->lock);
	return ret;
}

void cached_page_mark_uptodate(struct cached_page *cp)
{
	spinlock_acquire(&cp->lock);
	cp->flags |= PCP_UPTODATE;
	cp->flags &= ~PCP_ERROR;
	spinlock_release(&cp->lock);
}

void cached_page_mark_dirty(struct cached_page *cp)
{
	struct page_cache *m = cp->mapping;
	bool need_link;

	spinlock_acquire(&m->lock);
	spinlock_acquire(&cp->lock);
	need_link = !(cp->flags & PCP_DIRTY);
	cp->flags |= PCP_DIRTY;
	spinlock_release(&cp->lock);
	if (need_link)
		list_add_tail(&cp->dirty_list, &m->dirty_pages);
	spinlock_release(&m->lock);
}

struct cached_page *read_mapping_page(struct page_cache *m, pgoff_t index)
{
	struct cached_page *cp;
	int err;

	cp = find_or_create_page(m, index);
	if (!cp)
		return ERR_PTR(-ENOMEM);

	if (cached_page_uptodate(cp))
		return cp;

	cached_page_lock(cp);
	if (cached_page_uptodate(cp)) {
		cached_page_unlock(cp);
		return cp;
	}

	err = m->ops->read_page(m, cp);
	if (err) {
		spinlock_acquire(&cp->lock);
		cp->flags |= PCP_ERROR;
		spinlock_release(&cp->lock);
		cached_page_unlock(cp);
		cached_page_put(cp);
		return ERR_PTR(err);
	}
	cached_page_mark_uptodate(cp);
	cached_page_unlock(cp);
	return cp;
}

/*
 * Snapshot the dirty pages that overlap [first, last] onto @batch.
 *
 * Matching pages are moved off the mapping's dirty list and given an extra
 * reference (released by the writeback loop). They intentionally keep their
 * PCP_DIRTY marker while parked on @batch: cached_page_mark_dirty() only links
 * a page when PCP_DIRTY is clear, so a concurrent writer that re-dirties one
 * of these pages will not corrupt @batch by trying to re-link an already
 * linked node. Caller must hold @m->lock.
 */
static void collect_dirty_batch(struct page_cache *m, pgoff_t first,
				pgoff_t last, struct list_head *batch)
{
	struct cached_page *cp, *n;

	list_for_each_entry_safe(cp, n, &m->dirty_pages, dirty_list) {
		if (cp->index < first || cp->index > last)
			continue;
		refcnt_inc(&cp->refcnt);
		list_move_tail(&cp->dirty_list, batch);
	}
}

/*
 * Write a single page that was collected onto a writeback batch back to its
 * backing store, transitioning it dirty -> writeback -> clean.
 *
 * Clearing PCP_DIRTY (under both locks) is what lets a racing writer re-dirty
 * the page: once the flag is clear its dirty_list node is free again, so the
 * writer re-links it onto the mapping and a later flush will pick it up. This
 * is the standard "clear dirty before starting IO" ordering that avoids losing
 * an update that lands while writeback is in flight.
 */
static int writeback_one(struct page_cache *m, struct cached_page *cp)
{
	bool do_write;
	int err = 0;

	spinlock_acquire(&m->lock);
	list_del_init(&cp->dirty_list);
	spinlock_acquire(&cp->lock);
	do_write = (cp->flags & PCP_UPTODATE) != 0;
	cp->flags &= ~PCP_DIRTY;
	if (do_write)
		cp->flags |= PCP_WRITEBACK;
	spinlock_release(&cp->lock);
	spinlock_release(&m->lock);

	if (!do_write)
		return 0;

	cached_page_lock(cp);
	err = m->ops->write_page(m, cp);
	cached_page_unlock(cp);

	spinlock_acquire(&cp->lock);
	cp->flags &= ~PCP_WRITEBACK;
	if (err)
		cp->flags |= PCP_ERROR;
	spinlock_release(&cp->lock);

	/* Re-dirty so a later flush retries the page that failed to write. */
	if (err)
		cached_page_mark_dirty(cp);

	return err;
}

int page_cache_flush_range(struct page_cache *m, loff_t start, loff_t end)
{
	LIST_DEFINE(batch);
	pgoff_t first, last;
	int first_err = 0;

	if (!m || !m->ops || !m->ops->write_page)
		return 0;

	if (start < 0)
		start = 0;
	first = (pgoff_t)(start >> PAGE_SHIFT);
	last = (end < 0) ? ~(pgoff_t)0 : (pgoff_t)(end >> PAGE_SHIFT);

	spinlock_acquire(&m->lock);
	collect_dirty_batch(m, first, last, &batch);
	spinlock_release(&m->lock);

	while (!list_empty(&batch)) {
		struct cached_page *cp = list_first_entry(
			&batch, struct cached_page, dirty_list);
		int err;

		/* writeback_one() unlinks @cp from @batch. */
		err = writeback_one(m, cp);
		if (err && !first_err)
			first_err = err;
		cached_page_put(cp);
	}

	return first_err;
}

int page_cache_flush(struct page_cache *m)
{
	return page_cache_flush_range(m, 0, -1);
}

unsigned long page_cache_shrink(unsigned long nr_to_reclaim)
{
	unsigned long freed = 0;
	unsigned long scanned = 0;

	if (nr_to_reclaim == 0 || pagecache_nrpages == 0)
		return 0;

	while (freed < nr_to_reclaim && scanned < pagecache_nrpages) {
		struct page_cache *mapping;
		struct cached_page *cp;
		unsigned int flags;

		spinlock_acquire(&pagecache_lru_lock);
		if (list_empty(&pagecache_lru)) {
			spinlock_release(&pagecache_lru_lock);
			break;
		}
		cp = list_first_entry(&pagecache_lru, struct cached_page,
				      lru_list);
		list_del_init(&cp->lru_list);
		spinlock_release(&pagecache_lru_lock);

		scanned++;

		if (refcnt_read(&cp->refcnt) != 1) {
			pagecache_lru_touch(cp);
			continue;
		}

		mapping = cp->mapping;
		spinlock_acquire(&mapping->lock);
		flags = cp->flags;
		if (refcnt_read(&cp->refcnt) != 1 ||
		    (flags & (PCP_DIRTY | PCP_WRITEBACK)) ||
		    hlist_unhashed(&cp->ht_node)) {
			spinlock_release(&mapping->lock);
			pagecache_lru_touch(cp);
			continue;
		}
		__detach_locked(mapping, cp);
		spinlock_release(&mapping->lock);

		detach_and_put(cp);
		freed++;
	}

	return freed;
}

unsigned long page_cache_nr_pages(void)
{
	return pagecache_nrpages;
}

int truncate_inode_pages(struct page_cache *m, loff_t new_size)
{
	pgoff_t first_full;
	pgoff_t partial_idx;
	size_t partial_off;
	struct cached_page *cp;
	struct hlist_node *n;
	struct cached_page *partial = NULL;

	if (!m)
		return 0;
	if (new_size < 0)
		new_size = 0;

	first_full = (pgoff_t)((new_size + PAGE_SIZE - 1) >> PAGE_SHIFT);
	partial_idx = (pgoff_t)(new_size >> PAGE_SHIFT);
	partial_off = (size_t)(new_size & (PAGE_SIZE - 1));

	/* Phase 1: detach every page strictly past new_size from the cache.
	 * Detaching drops the cache's implicit reference; any user references
	 * still outstanding will keep the page alive until their put. */
	spinlock_acquire(&m->lock);
	for (size_t i = 0; i < PAGE_CACHE_HSIZE; ++i) {
		hlist_for_each_entry_safe(cp, n, &m->pages[i], ht_node) {
			if (cp->index < first_full)
				continue;
			__detach_locked(m, cp);
			spinlock_release(&m->lock);
			detach_and_put(cp);
			spinlock_acquire(&m->lock);
		}
	}
	spinlock_release(&m->lock);

	/* Phase 2: zero the tail of the page that straddles new_size. */
	if (partial_off) {
		partial = read_mapping_page(m, partial_idx);
		if (IS_ERR(partial))
			return PTR_ERR(partial);

		cached_page_lock(partial);
		memset((uint8_t *)cached_page_addr(partial) + partial_off, 0,
		       PAGE_SIZE - partial_off);
		cached_page_unlock(partial);
		cached_page_mark_dirty(partial);
		cached_page_put(partial);
	}

	return 0;
}

ssize_t generic_file_read(struct fs_file *file, char *buf, size_t size,
			  loff_t *pos)
{
	struct fs_inode *inode = file->inode;
	struct page_cache *m = inode->mapping;
	ssize_t total = 0;
	loff_t isize;
	loff_t end;
	int err = 0;

	if (!m)
		return -EIO;

	sleeplock_acquire(&inode->rwsem);

	isize = inode->size;
	if (*pos >= isize) {
		sleeplock_release(&inode->rwsem);
		return 0;
	}
	end = *pos + (loff_t)size;
	if (end > isize)
		size = (size_t)(isize - *pos);

	while (size > 0) {
		pgoff_t index = (pgoff_t)(*pos >> PAGE_SHIFT);
		size_t off = (size_t)(*pos & (PAGE_SIZE - 1));
		size_t nr = PAGE_SIZE - off;
		struct cached_page *cp;

		if (nr > size)
			nr = size;

		cp = read_mapping_page(m, index);
		if (IS_ERR(cp)) {
			err = PTR_ERR(cp);
			break;
		}

		cached_page_lock(cp);
		memcpy(buf, (uint8_t *)cached_page_addr(cp) + off, nr);
		cached_page_unlock(cp);
		cached_page_put(cp);

		buf += nr;
		*pos += (loff_t)nr;
		size -= nr;
		total += (ssize_t)nr;
	}

	sleeplock_release(&inode->rwsem);
	return total > 0 ? total : err;
}

ssize_t generic_file_write(struct fs_file *file, const char *buf, size_t size,
			   loff_t *pos)
{
	struct fs_inode *inode = file->inode;
	struct page_cache *m = inode->mapping;
	ssize_t total = 0;
	int err = 0;

	if (!m)
		return -EIO;

	sleeplock_acquire(&inode->rwsem);

	while (size > 0) {
		pgoff_t index = (pgoff_t)(*pos >> PAGE_SHIFT);
		size_t off = (size_t)(*pos & (PAGE_SIZE - 1));
		size_t nr = PAGE_SIZE - off;
		struct cached_page *cp;
		bool full_page = (off == 0 && nr >= PAGE_SIZE);

		if (nr > size)
			nr = size;

		/* Full-page writes can skip readpage; partial writes must
		 * read-modify-write so the surrounding bytes are preserved
		 * across writeback. */
		if (full_page && size >= PAGE_SIZE) {
			cp = find_or_create_page(m, index);
			if (!cp) {
				err = -ENOMEM;
				break;
			}
			cached_page_lock(cp);
			memcpy((char *)cached_page_addr(cp), buf, PAGE_SIZE);
			cached_page_mark_uptodate(cp);
			cached_page_unlock(cp);
			nr = PAGE_SIZE;
		} else {
			cp = read_mapping_page(m, index);
			if (IS_ERR(cp)) {
				err = PTR_ERR(cp);
				break;
			}
			cached_page_lock(cp);
			memcpy((char *)cached_page_addr(cp) + off, buf, nr);
			cached_page_unlock(cp);
		}

		cached_page_mark_dirty(cp);
		cached_page_put(cp);

		buf += nr;
		*pos += (loff_t)nr;
		size -= nr;
		total += (ssize_t)nr;

		if (*pos > inode->size)
			inode->size = *pos;
	}

	if (total > 0) {
		inode_touch_mtime(inode);
		fs_inode_mark_dirty(inode);
	}

	sleeplock_release(&inode->rwsem);
	return total > 0 ? total : err;
}
