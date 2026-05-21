#ifndef BRK_PAGECACHE_H
#define BRK_PAGECACHE_H

#include <brk/fs_types.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/refcnt.h>
#include <brk/types.h>

typedef unsigned long pgoff_t;

#define PCP_UPTODATE (1u << 0) /* page contents are valid */
#define PCP_DIRTY (1u << 1) /* page is on the mapping's dirty list */
#define PCP_ERROR (1u << 2) /* last IO returned an error */

#define ADDRESS_SPACE_HBITS 6
#define ADDRESS_SPACE_HSIZE (1u << ADDRESS_SPACE_HBITS)

struct address_space;
struct address_space_operations;

/*
 * A cached page lives in exactly one address_space at a time.
 *
 * Lifetime / locking:
 *   - @refcnt is the user reference count. The cache itself does NOT take
 *     a reference. When @refcnt drops to zero and the page is not dirty,
 *     the page is detached from the mapping and freed.
 *   - @flags is protected by @lock.
 *   - @io_lock serializes ->readpage / ->writepage on the page.
 *   - @ht_node / @dirty_list are protected by mapping->lock.
 */
struct cached_page {
	refcnt_t refcnt;
	struct page *page;
	struct address_space *mapping;
	pgoff_t index;
	unsigned int flags;
	spinlock_t lock;
	sleeplock_t io_lock;
	struct hlist_node ht_node;
	struct list_head dirty_list;
};

struct address_space_operations {
	/**
	 * readpage() - populate @cp->page from backing store
	 *
	 * Called with @cp->io_lock held. On success the implementation must
	 * leave the page contents valid (zero-filling holes is acceptable);
	 * the caller marks the page up-to-date.
	 *
	 * Return: 0 on success, negative errno on failure.
	 */
	int (*readpage)(struct address_space *mapping, struct cached_page *cp);

	/**
	 * writepage() - persist @cp->page to backing store
	 *
	 * Called with @cp->io_lock held. The implementation should write the
	 * entire page to the appropriate disk block, allocating one on demand
	 * if the file has a hole there.
	 *
	 * Return: 0 on success, negative errno on failure.
	 */
	int (*writepage)(struct address_space *mapping, struct cached_page *cp);
};

/*
 * @host is opaque so a mapping can be backed by either a file inode
 * (struct inode *) or a block device (struct blkdev *). Callbacks in
 * a_ops know which one to cast to.
 */
struct address_space {
	void *host;
	const struct address_space_operations *a_ops;

	spinlock_t lock;
	struct hlist_head pages[ADDRESS_SPACE_HSIZE];
	struct list_head dirty_pages;
	unsigned long nrpages;
};

void pagecache_init(void);

struct address_space *
address_space_alloc(void *host, const struct address_space_operations *a_ops);
void address_space_free(struct address_space *mapping);

/* Lookup helpers — each returns the page with @refcnt incremented. */
struct cached_page *find_get_page(struct address_space *mapping, pgoff_t index);
struct cached_page *find_or_create_page(struct address_space *mapping,
					pgoff_t index);
/*
 * read_mapping_page - return a fully populated cached page.
 *
 * Locks the page, invokes ->readpage if necessary, marks it up-to-date,
 * unlocks it and returns with an extra reference held.
 *
 * Return: cached_page on success, ERR_PTR(-errno) on failure.
 */
struct cached_page *read_mapping_page(struct address_space *mapping,
				      pgoff_t index);

void cached_page_get(struct cached_page *cp);
void cached_page_put(struct cached_page *cp);

void cached_page_lock(struct cached_page *cp);
void cached_page_unlock(struct cached_page *cp);

void cached_page_mark_dirty(struct cached_page *cp);
void cached_page_mark_uptodate(struct cached_page *cp);
bool cached_page_uptodate(struct cached_page *cp);
bool cached_page_dirty(struct cached_page *cp);

/*
 * filemap_writeback - flush every dirty page in @mapping via ->writepage.
 *
 * Pages remain in the cache after a successful writeback. Returns 0 on
 * success or the first negative errno encountered.
 */
int filemap_writeback(struct address_space *mapping);

/*
 * truncate_inode_pages - drop cached pages at or beyond @new_size.
 *
 * Pages strictly past @new_size are removed from the cache without
 * writeback. The page that straddles @new_size (partial tail) is
 * zero-filled past the truncation point and marked dirty, so that a
 * later writeback persists the truncation.
 */
void truncate_inode_pages(struct address_space *mapping, loff_t new_size);

/* Generic file I/O helpers built on top of the page cache. */
ssize_t generic_file_read(struct file *file, char *buf, usize_t size,
			  loff_t *pos);
ssize_t generic_file_write(struct file *file, const char *buf, usize_t size,
			   loff_t *pos);

static inline void *cached_page_addr(const struct cached_page *cp)
{
	return (void *)page_to_virt(cp->page);
}

#endif
