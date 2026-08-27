#ifndef BRK_SLAB_H
#define BRK_SLAB_H

#include <brk/lock/spinlock_types.h>
#include <brk/mm/mm_types.h>

struct slab_allocator {
	const char *name;
	size_t size;
	size_t align;
	struct list_head slab_list;
	unsigned int page_order;
	spinlock_t lock;
};

int slab_init(struct slab_allocator *allocator, size_t size, size_t align,
	      const char *name);
void slab_deinit(struct slab_allocator *allocator);
void *slab_alloc(struct slab_allocator *allocator);
void *slab_alloc_zero(struct slab_allocator *allocator);
void slab_free(struct slab_allocator *allocator, void *obj);

#endif
