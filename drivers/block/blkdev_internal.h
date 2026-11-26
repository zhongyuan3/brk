#ifndef _BLKDEV_INTERNAL_H
#define _BLKDEV_INTERNAL_H

#include <brk/pagecache.h>

/* Page-cache operations used by blkdev_alloc(). */
extern const struct page_cache_ops bdev_pc_ops;

#endif
