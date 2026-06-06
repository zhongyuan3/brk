#ifndef BRK_PGALLOC_H
#define BRK_PGALLOC_H

#include <brk/mm_types.h>

#define PAGE_ORDER_MAX 12

#define PAGE_FLAGS_BUDDY (1 << 0) /* Is this page in the buddy system? */
#define PAGE_FLAGS_HEAD (1 << 1) /* Is this page the head of a block? */
#define PAGE_FLAGS_SLAB (1 << 2) /* Is this page in the SLAB system? */
#define PAGE_FLAGS_FREE (1 << 3) /* Is this page free? */
#define PAGE_FLAGS_KMALLOC (1 << 4) /* Buddy block from kmalloc() > max slab */

#define PAGE_FLAGS_NEW_PAGE (PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD)

#define PAGE_FLAGS_NEW_FREE_PAGE \
	(PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD | PAGE_FLAGS_FREE)

void page_alloc_init(void);
struct page *page_alloc(unsigned int order);
struct page *page_zalloc(unsigned int order);
void page_free(struct page *pg, unsigned int order);

#endif
