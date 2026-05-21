#ifndef BRK_SLAB_H
#define BRK_SLAB_H

#include <brk/lock.h>
#include <brk/mm_types.h>

#define NR_KMALLOC_CACHES 10

struct kobj_pool {
	const char *name;
	usize_t size;
	usize_t align;
	struct list_head slab_list;
	unsigned int page_order;
	spinlock_t lock;
};

int kmem_cache_init(struct kobj_pool *cache, usize_t size, usize_t align,
		    const char *name);
void kmem_cache_deinit(struct kobj_pool *cache);
void *kmem_cache_alloc(struct kobj_pool *cache);
void kmem_cache_free(struct kobj_pool *cache, void *obj);

void kmalloc_init(void);
void *kmalloc(usize_t size);
void *kcalloc(usize_t nmemb, usize_t size);
void *kzalloc(usize_t size);
void kfree(void *ptr);

#endif
