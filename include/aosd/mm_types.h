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

struct kmem_cache {
	const char *name;
	size_t size;
	size_t align;
	struct list_head slab_list;
	unsigned int page_order;
};

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
	};
};

struct free_area {
	struct list_head free_list;
};

#endif
