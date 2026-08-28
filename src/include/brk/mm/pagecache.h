#ifndef BRK_PAGECACHE_H
#define BRK_PAGECACHE_H

#include <brk/base/list.h>
#include <brk/base/types.h>
#include <brk/fs/fs_types.h>
#include <brk/lib/refcnt_types.h>
#include <brk/lock/sleeplock_types.h>
#include <brk/lock/spinlock_types.h>
#include <brk/mm/mm_types.h>
#include <brk/mm/pgalloc.h>

typedef unsigned long pgoff_t;

#define PCP_UPTODATE (1u << 0) /* page contents are valid */
#define PCP_DIRTY (1u << 1) /* page is on the mapping's dirty list */
#define PCP_ERROR (1u << 2) /* last IO returned an error */
#define PCP_WRITEBACK (1u << 3) /* writeback IO is currently in flight */

#define PAGE_CACHE_HBITS 6
#define PAGE_CACHE_HSIZE (1u << PAGE_CACHE_HBITS)

struct page_cache;
struct page_cache_ops;

struct cached_page {
	refcnt_t refcnt;
	struct page *page;
	struct page_cache *mapping;
	pgoff_t index;
	unsigned int flags;
	spinlock_t lock;
	sleeplock_t io_lock;
	struct hlist_node ht_node;
	struct list_head dirty_list;
	struct list_head lru_list;
};

struct page_cache_ops {
	/**
	 * read_page() - populate @cp->page from backing store
	 *
	 * Called with @cp->io_lock held. On success the implementation must
	 * leave the page contents valid (zero-filling holes is acceptable);
	 * the caller marks the page up-to-date.
	 *
	 * Return: 0 on success, negative errno on failure.
	 */
	int (*read_page)(struct page_cache *mapping, struct cached_page *cp);

	/**
	 * write_page() - persist @cp->page to backing store
	 *
	 * Called with @cp->io_lock held. The implementation should write the
	 * entire page to the appropriate disk block, allocating one on demand
	 * if the file has a hole there.
	 *
	 * Return: 0 on success, negative errno on failure.
	 */
	int (*write_page)(struct page_cache *mapping, struct cached_page *cp);
};

struct page_cache {
	void *host;
	const struct page_cache_ops *ops;

	spinlock_t lock;
	struct hlist_head pages[PAGE_CACHE_HSIZE];
	struct list_head dirty_pages;
	unsigned long nrpages;
};

void page_cache_init(void);

/*
 * page_cache_shrink() - reclaim clean, unreferenced cached pages.
 *
 * Walks the global LRU (oldest first) and evicts up to @nr_to_reclaim pages
 * that are clean, not under writeback, and held only by the cache itself
 * (refcount == 1). Dirty pages are skipped and rotated to the MRU end.
 *
 * Return: the number of pages actually freed.
 */
unsigned long page_cache_shrink(unsigned long nr_to_reclaim);

/* Return the number of pages currently tracked on the global LRU. */
unsigned long page_cache_nr_pages(void);

struct page_cache *page_cache_create(void *host,
				     const struct page_cache_ops *ops);
void page_cache_destroy(struct page_cache *mapping);

/*
 * read_mapping_page() - return a fully populated cached page.
 *
 * Locks the page, invokes ->read_page if necessary, marks it up-to-date,
 * unlocks it and returns with an extra reference held.
 *
 * Return: cached_page on success, ERR_PTR(-errno) on failure.
 */
struct cached_page *read_mapping_page(struct page_cache *mapping,
				      pgoff_t index);

void cached_page_get(struct cached_page *cp);
void cached_page_put(struct cached_page *cp);

void cached_page_lock(struct cached_page *cp);
void cached_page_unlock(struct cached_page *cp);

void cached_page_mark_dirty(struct cached_page *cp);
void cached_page_mark_uptodate(struct cached_page *cp);
bool cached_page_uptodate(struct cached_page *cp);
bool cached_page_dirty(struct cached_page *cp);
bool cached_page_writeback(struct cached_page *cp);

/*
 * page_cache_flush_range() - write back dirty pages overlapping a byte range.
 *
 * Flushes every dirty page whose contents intersect the inclusive byte range
 * [@start, @end]. A negative @end means "up to the end of the mapping".
 *
 * The set of pages to write is snapshotted under the mapping lock before any
 * IO is issued, so the call is guaranteed to terminate even if other threads
 * keep dirtying pages concurrently; such pages are simply left for a later
 * flush. Pages remain in the cache after a successful writeback.
 *
 * Return: 0 on success, or the first negative errno encountered (pages that
 * failed to write are left dirty so a subsequent flush retries them).
 */
int page_cache_flush_range(struct page_cache *mapping, loff_t start,
			   loff_t end);

/*
 * page_cache_flush() - flush every dirty page in @mapping.
 *
 * Equivalent to page_cache_flush_range() over the whole mapping.
 */
int page_cache_flush(struct page_cache *mapping);

/*
 * truncate_inode_pages - drop cached pages at or beyond @new_size.
 *
 * Pages strictly past @new_size are removed from the cache without
 * writeback. When @new_size falls inside a page, that page is read in if
 * needed, zero-filled from the truncation point to the end of the page,
 * and marked dirty. The caller must run page_cache_flush() before freeing
 * on-disk blocks so the partial tail and other dirty pages are persisted.
 *
 * Return: 0 on success, negative errno on failure.
 */
int truncate_inode_pages(struct page_cache *mapping, loff_t new_size);

/* Generic file I/O helpers built on top of the page cache. */
ssize_t generic_file_read(struct fs_file *file, char *buf, size_t size,
			  loff_t *pos);
ssize_t generic_file_write(struct fs_file *file, const char *buf, size_t size,
			   loff_t *pos);

static inline void *cached_page_addr(const struct cached_page *cp)
{
	return (void *)page_to_virt(cp->page);
}

#endif
