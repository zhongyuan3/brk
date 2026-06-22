#ifndef BRK_MM_TYPES_H
#define BRK_MM_TYPES_H

#include <arch/pgtable_types.h>
#include <brk/refcnt_types.h>
#include <brk/types.h>

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
			usize_t slab_free_count;
			usize_t slab_objs_count;
		};

		struct { /* tmpfs */
			struct page *tmpfs_next;
		};
	};
};

struct uvm_region {
	struct list_head list;
	u64 addr;
	usize_t size;
	struct page **pages;
	usize_t nr_pages;
	unsigned int flags;
};

struct uvm_space {
	pgde_t *pgd;
	struct list_head seg;
	struct uvm_region *stack;
	struct uvm_region *heap;
	u64 brk;
	refcnt_t refcnt;
};

#endif
