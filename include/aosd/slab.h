#ifndef AOSD_SLAB_H
#define AOSD_SLAB_H

#include <aosd/lock.h>
#include <aosd/mm_types.h>

#define NR_KMALLOC_CACHES 10

struct kmem_cache {
	const char *name;
	size_t size;
	size_t align;
	struct list_head slab_list;
	unsigned int page_order;
	spinlock_t lock;
};

int kmem_cache_init(struct kmem_cache *cache, size_t size, size_t align,
		    const char *name);
void kmem_cache_deinit(struct kmem_cache *cache);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *obj);

void kmalloc_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

#endif
