#include <brk/assert.h>
#include <brk/errno.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/pagecache.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/string.h>

/*
 * Page cache (a.k.a. address_space) implementation.
 *
 * Locking summary
 * ---------------
 *   mapping->lock      protects mapping->pages[] (hash buckets),
 *                      mapping->dirty_pages and mapping->nrpages.
 *   cached_page->lock  protects cached_page->flags.
 *   cached_page->io_lock  serializes ->readpage / ->writepage on a single
 *                      page (held while talking to the backing store).
 *
 * Lock ordering: mapping->lock -> cached_page->lock. The io_lock is a
 * sleeplock and is never held while taking either of the spinlocks.
 *
 * Refcount rules
 * --------------
 *   While a cached_page is reachable from the cache (i.e. linked into the
 *   hash) it holds one implicit reference (the "cache reference"). Each
 *   successful find_get_page() / find_or_create_page() returns a fresh
 *   user reference on top of that.
 *
 *   The cache reference is dropped by the code path that removes the page
 *   from the hash (truncate_inode_pages() and address_space_free()). When
 *   the last user reference is then released, cached_page_put() frees the
 *   page. Because removal from the hash always happens before the cache's
 *   ref is dropped, no concurrent lookup can resurrect an already-detached
 *   page.
 */

static struct kmem_cache cached_page_cache;

void pagecache_init(void)
{
	kmem_cache_init(&cached_page_cache, sizeof(struct cached_page),
			alignof(struct cached_page), "cached_page");
}

static unsigned int mapping_hash(pgoff_t index)
{
	u64 h = (u64)index;
	h *= 0x9E3779B97F4A7C15ULL;
	h ^= h >> 32;
	return (unsigned int)(h & (ADDRESS_SPACE_HSIZE - 1));
}

struct address_space *
address_space_alloc(void *host, const struct address_space_operations *a_ops)
{
	struct address_space *m;

	m = kzalloc(sizeof(*m));
	if (!m)
		return NULL;

	m->host = host;
	m->a_ops = a_ops;
	spinlock_init(&m->lock, "address_space.lock");
	for (usize_t i = 0; i < ADDRESS_SPACE_HSIZE; ++i)
		hlist_head_init(&m->pages[i]);
	list_init(&m->dirty_pages);
	m->nrpages = 0;
	return m;
}

/* Caller must hold @m->lock. */
static void __detach_locked(struct address_space *m, struct cached_page *cp)
{
	hlist_del_init(&cp->ht_node);
	if (cp->flags & PCP_DIRTY) {
		list_del_init(&cp->dirty_list);
		cp->flags &= ~PCP_DIRTY;
	}
	m->nrpages--;
}

/* Drop the implicit cache reference for a page that has just been removed
 * from the hash. May free the page. */
static void detach_and_put(struct cached_page *cp)
{
	cached_page_put(cp);
}

void address_space_free(struct address_space *m)
{
	struct cached_page *cp;
	struct hlist_node *n;

	if (!m)
		return;

	spinlock_acquire(&m->lock);
	for (usize_t i = 0; i < ADDRESS_SPACE_HSIZE; ++i) {
		hlist_for_each_entry_safe(cp, n, &m->pages[i], ht_node)
		{
			__detach_locked(m, cp);
			spinlock_release(&m->lock);
			detach_and_put(cp);
			spinlock_acquire(&m->lock);
		}
	}
	spinlock_release(&m->lock);

	kfree(m);
}

static struct cached_page *cached_page_alloc(struct address_space *m,
					     pgoff_t index)
{
	struct cached_page *cp;
	struct page *pg;

	cp = kmem_cache_alloc(&cached_page_cache);
	if (!cp)
		return NULL;
	pg = page_zalloc(0);
	if (!pg) {
		kmem_cache_free(&cached_page_cache, cp);
		return NULL;
	}

	/* Refcount starts at 1 for the cache's implicit reference. The user
	 * reference will be added by the insertion path below. */
	arc_init(&cp->refcnt, 1);
	cp->page = pg;
	cp->mapping = m;
	cp->index = index;
	cp->flags = 0;
	spinlock_init(&cp->lock, "cached_page.lock");
	sleeplock_init(&cp->io_lock, "cached_page.io_lock");
	hlist_node_init(&cp->ht_node);
	list_init(&cp->dirty_list);
	return cp;
}

static void cached_page_destroy(struct cached_page *cp)
{
	page_free(cp->page, 0);
	kmem_cache_free(&cached_page_cache, cp);
}

static struct cached_page *__lookup_locked(struct address_space *m,
					   pgoff_t index)
{
	struct hlist_head *bucket = &m->pages[mapping_hash(index)];
	struct cached_page *cp;

	hlist_for_each_entry(cp, bucket, ht_node) {
		if (cp->index == index)
			return cp;
	}
	return NULL;
}

struct cached_page *find_get_page(struct address_space *m, pgoff_t index)
{
	struct cached_page *cp;

	spinlock_acquire(&m->lock);
	cp = __lookup_locked(m, index);
	if (cp)
		arc_inc(&cp->refcnt);
	spinlock_release(&m->lock);
	return cp;
}

struct cached_page *find_or_create_page(struct address_space *m, pgoff_t index)
{
	struct cached_page *cp, *existing;

	cp = find_get_page(m, index);
	if (cp)
		return cp;

	cp = cached_page_alloc(m, index);
	if (!cp)
		return NULL;

	spinlock_acquire(&m->lock);
	existing = __lookup_locked(m, index);
	if (existing) {
		arc_inc(&existing->refcnt);
		spinlock_release(&m->lock);
		cached_page_destroy(cp);
		return existing;
	}
	hlist_add_head(&cp->ht_node, &m->pages[mapping_hash(index)]);
	m->nrpages++;
	arc_inc(&cp->refcnt); /* user reference returned to caller */
	spinlock_release(&m->lock);

	return cp;
}

void cached_page_get(struct cached_page *cp)
{
	arc_inc(&cp->refcnt);
}

void cached_page_put(struct cached_page *cp)
{
	if (!cp)
		return;
	if (arc_dec_fetch(&cp->refcnt) > 0)
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

void cached_page_mark_uptodate(struct cached_page *cp)
{
	spinlock_acquire(&cp->lock);
	cp->flags |= PCP_UPTODATE;
	cp->flags &= ~PCP_ERROR;
	spinlock_release(&cp->lock);
}

void cached_page_mark_dirty(struct cached_page *cp)
{
	struct address_space *m = cp->mapping;
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

struct cached_page *read_mapping_page(struct address_space *m, pgoff_t index)
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

	err = m->a_ops->readpage(m, cp);
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

int filemap_writeback(struct address_space *m)
{
	int first_err = 0;

	if (!m || !m->a_ops || !m->a_ops->writepage)
		return 0;

	while (1) {
		struct cached_page *cp = NULL;
		bool was_dirty = false;
		bool was_uptodate = false;
		int err = 0;

		spinlock_acquire(&m->lock);
		if (!list_empty(&m->dirty_pages)) {
			cp = list_first_entry(&m->dirty_pages,
					      struct cached_page, dirty_list);
			arc_inc(&cp->refcnt);
			list_del_init(&cp->dirty_list);
			spinlock_acquire(&cp->lock);
			was_dirty = (cp->flags & PCP_DIRTY) != 0;
			was_uptodate = (cp->flags & PCP_UPTODATE) != 0;
			cp->flags &= ~PCP_DIRTY;
			spinlock_release(&cp->lock);
		}
		spinlock_release(&m->lock);

		if (!cp)
			break;

		if (was_dirty && was_uptodate) {
			cached_page_lock(cp);
			err = m->a_ops->writepage(m, cp);
			cached_page_unlock(cp);
		}

		if (err) {
			cached_page_mark_dirty(cp);
			if (!first_err)
				first_err = err;
		}
		cached_page_put(cp);
	}

	return first_err;
}

void truncate_inode_pages(struct address_space *m, loff_t new_size)
{
	pgoff_t first_full;
	pgoff_t partial_idx;
	usize_t partial_off;
	struct cached_page *cp;
	struct hlist_node *n;
	struct cached_page *partial = NULL;

	if (!m)
		return;
	if (new_size < 0)
		new_size = 0;

	first_full = (pgoff_t)((new_size + PAGE_SIZE - 1) >> PAGE_SHIFT);
	partial_idx = (pgoff_t)(new_size >> PAGE_SHIFT);
	partial_off = (usize_t)(new_size & (PAGE_SIZE - 1));

	/* Phase 1: detach every page strictly past new_size from the cache.
	 * Detaching drops the cache's implicit reference; any user references
	 * still outstanding will keep the page alive until their put. */
	spinlock_acquire(&m->lock);
	for (usize_t i = 0; i < ADDRESS_SPACE_HSIZE; ++i) {
		hlist_for_each_entry_safe(cp, n, &m->pages[i], ht_node)
		{
			if (cp->index < first_full)
				continue;
			__detach_locked(m, cp);
			spinlock_release(&m->lock);
			detach_and_put(cp);
			spinlock_acquire(&m->lock);
		}
	}
	if (partial_off) {
		partial = __lookup_locked(m, partial_idx);
		if (partial)
			arc_inc(&partial->refcnt);
	}
	spinlock_release(&m->lock);

	/* Phase 2: zero the tail of the last (partial) page if any. */
	if (partial) {
		cached_page_lock(partial);
		if (cached_page_uptodate(partial)) {
			void *base = cached_page_addr(partial);
			memset((char *)base + partial_off, 0,
			       PAGE_SIZE - partial_off);
			cached_page_unlock(partial);
			cached_page_mark_dirty(partial);
		} else {
			cached_page_unlock(partial);
		}
		cached_page_put(partial);
	}
}

ssize_t generic_file_read(struct file *file, char *buf, usize_t size,
			  loff_t *pos)
{
	struct inode *inode = file->f_inode;
	struct address_space *m = inode->i_mapping;
	ssize_t total = 0;
	loff_t isize;
	loff_t end;
	int err = 0;

	if (!m)
		return -EIO;

	sleeplock_acquire(&inode->i_rwsem);

	isize = inode->i_size;
	if (*pos >= isize) {
		sleeplock_release(&inode->i_rwsem);
		return 0;
	}
	end = *pos + (loff_t)size;
	if (end > isize)
		size = (usize_t)(isize - *pos);

	while (size > 0) {
		pgoff_t index = (pgoff_t)(*pos >> PAGE_SHIFT);
		usize_t off = (usize_t)(*pos & (PAGE_SIZE - 1));
		usize_t nr = PAGE_SIZE - off;
		struct cached_page *cp;

		if (nr > size)
			nr = size;

		cp = read_mapping_page(m, index);
		if (IS_ERR(cp)) {
			err = PTR_ERR(cp);
			break;
		}

		memcpy(buf, (char *)cached_page_addr(cp) + off, nr);
		cached_page_put(cp);

		buf += nr;
		*pos += (loff_t)nr;
		size -= nr;
		total += (ssize_t)nr;
	}

	sleeplock_release(&inode->i_rwsem);
	return total > 0 ? total : err;
}

ssize_t generic_file_write(struct file *file, const char *buf, usize_t size,
			   loff_t *pos)
{
	struct inode *inode = file->f_inode;
	struct address_space *m = inode->i_mapping;
	ssize_t total = 0;
	int err = 0;

	if (!m)
		return -EIO;

	sleeplock_acquire(&inode->i_rwsem);

	while (size > 0) {
		pgoff_t index = (pgoff_t)(*pos >> PAGE_SHIFT);
		usize_t off = (usize_t)(*pos & (PAGE_SIZE - 1));
		usize_t nr = PAGE_SIZE - off;
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

		if (*pos > inode->i_size)
			inode->i_size = *pos;
	}

	if (total > 0) {
		inode_touch_mtime(inode);
		inode_mark_dirty(inode);
	}

	sleeplock_release(&inode->i_rwsem);
	return total > 0 ? total : err;
}
