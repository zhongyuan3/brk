#ifndef BRK_MM_TYPES_H
#define BRK_MM_TYPES_H

#include <arch/pgtable_types.h>
#include <brk/base/types.h>
#include <brk/lib/refcnt_types.h>

struct page {
	unsigned int flags;

	union {
		struct { /* Buddy */
			struct list_head buddy_lru;
			/* Only valid when PAGE_FLAGS_HEAD is set */
			unsigned int buddy_page_order;
		};

		struct { /* SLAB */
			void *slab_free_objs;
			struct slab_allocator *slab_cache;
			struct list_head slab_list;
			size_t slab_free_count;
			size_t slab_objs_count;
		};

		struct { /* tmpfs */
			struct page *tmpfs_next;
		};
	};
};

struct uvm_region {
	struct list_head list;
	uint64_t addr;
	size_t size;
	struct page **pages;
	size_t nr_pages;
	unsigned int flags;
};

struct uvm_space {
	pgd_t *pgd;
	struct list_head seg;
	struct uvm_region *stack;
	struct uvm_region *heap;
	uint64_t brk;
	refcnt_t refcnt;
};

#endif
