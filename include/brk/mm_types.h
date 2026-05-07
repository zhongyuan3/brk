#ifndef BRK_MM_TYPES_H
#define BRK_MM_TYPES_H

#include <brk/types.h>

typedef struct {
	uint64_t pgde;
} pgde_t;

typedef struct {
	uint64_t pmde;
} pmde_t;

typedef struct {
	uint64_t pte;
} pte_t;

struct kmem_cache;

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
			struct kmem_cache *slab_cache;
			struct list_head slab_list;
			size_t slab_free_count;
			size_t slab_objs_count;
		};

		struct { /* tmpfs */
			struct page *tmpfs_next;
		};
	};
};

struct free_area {
	struct list_head free_list;
};

struct vm_area;

struct mm_struct {
	pgde_t *pgd;
	struct list_head seg;
	struct vm_area *stack;
	struct vm_area *heap;
	uint64_t brk;
};

#endif
