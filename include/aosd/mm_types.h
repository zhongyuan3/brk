#ifndef AOSD_MM_TYPES_H
#define AOSD_MM_TYPES_H

#include <aosd/types.h>

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
			struct list_head lru;
			/* Only valid when PAGE_FLAGS_HEAD is set */
			unsigned int order;
		};

		struct { /* SLUB */
			void *free_list;
			struct kmem_cache *cache;
			struct list_head slub_list;
			size_t free_count;
			size_t object_count;
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
