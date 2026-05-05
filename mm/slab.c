#include <brk/assert.h>
#include <brk/errno.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/lock.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/string.h>

static struct kmem_cache kmalloc_caches[NR_KMALLOC_CACHES];

static int kmalloc_cache_index(size_t size)
{
	if (size <= 8)
		return 0;
	else if (size <= 16)
		return 1;
	else if (size <= 32)
		return 2;
	else if (size <= 64)
		return 3;
	else if (size <= 128)
		return 4;
	else if (size <= 256)
		return 5;
	else if (size <= 512)
		return 6;
	else if (size <= 1024)
		return 7;
	else if (size <= 2048)
		return 8;
	else if (size <= 4096)
		return 9;
	else
		return -1;
}

void kmalloc_init(void)
{
	size_t sz[NR_KMALLOC_CACHES] = {
		8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
	};
	size_t align = alignof(max_align_t);
	for (int i = 0; i < NR_KMALLOC_CACHES; ++i)
		kmem_cache_init(&kmalloc_caches[i], sz[i], align, "kmalloc");
}

void *kmalloc(size_t size)
{
	if (size == 0)
		return NULL;

	int index = kmalloc_cache_index(size);
	if (index >= 0)
		return kmem_cache_alloc(&kmalloc_caches[index]);

	struct page *pg = page_alloc(page_order(size));
	if (!pg)
		return NULL;

	return (void *)page_to_virt(pg);
}

void *kcalloc(size_t nmemb, size_t size)
{
	void *ptr = kmalloc(nmemb * size);
	if (!ptr)
		return NULL;

	memset(ptr, 0, nmemb * size);
	return ptr;
}

void *kzalloc(size_t size)
{
	return kcalloc(1, size);
}

void kfree(void *ptr)
{
	if (!ptr)
		return;
	struct page *pg = virt_to_page((uint64_t)ptr);
	if (pg->flags & PAGE_FLAGS_SLUB) {
		kmem_cache_free(pg->cache, ptr);
	} else {
		ASSERT(pg);
		page_free(pg, pg->order);
	}
}

static int kmem_cache_add_page(struct kmem_cache *cache)
{
	size_t align = cache->align;
	size_t size = cache->size;

	struct page *pg = page_alloc(cache->page_order);
	if (!pg)
		return -ENOMEM;

	pg->flags |= PAGE_FLAGS_SLUB;
	pg->cache = cache;

	uint64_t addr = page_to_virt(pg);
	uint64_t end_addr = addr + (1 << (PAGE_SHIFT + cache->page_order));
	if (!is_aligned(addr, align))
		addr = round_up(addr, align);

	pg->free_list = (void *)addr;

	pg->object_count = pg->free_count = (end_addr - addr) / size;
	while (addr + size < end_addr) {
		void **curr_next_ptr = (void **)addr;
		void *next = (void *)(addr + size);
		*curr_next_ptr = next;
		addr += size;
	}
	*(void **)addr = NULL;

	list_add(&pg->slub_list, &cache->slab_list);

	return 0;
}

int kmem_cache_init(struct kmem_cache *cache, size_t size, size_t align,
		    const char *name)
{
	int ret;

	if (size == 0 || align == 0)
		return -EINVAL;

	if (!is_power_of_two(align))
		align = round_up_to_pow_of_two(align);

	if (!is_aligned(size, align))
		size = round_up(size, align);

	cache->size = size;
	cache->align = align;
	cache->page_order = page_order(size);
	cache->name = name;
	spinlock_init(&cache->lock, name);

	list_init(&cache->slab_list);

	spinlock_acquire(&cache->lock);
	ret = kmem_cache_add_page(cache);
	spinlock_release(&cache->lock);
	return ret;
}

void kmem_cache_deinit(struct kmem_cache *cache)
{
	struct list_head *first;
	struct page *pg;
	struct list_head *list;

	spinlock_acquire(&cache->lock);
	list = &cache->slab_list;
	while (!list_empty(list)) {
		first = list->next;
		list_del(first);
		pg = list_entry(first, struct page, slub_list);
		ASSERT(pg->free_count == pg->object_count);
		pg->flags &= ~PAGE_FLAGS_SLUB;
		ASSERT(pg);
		page_free(pg, 0);
	}
	spinlock_release(&cache->lock);
}

void *kmem_cache_alloc(struct kmem_cache *cache)
{
	struct page *curr;
	void *obj;
	int attempt = 0;
	struct list_head *list;

	spinlock_acquire(&cache->lock);
	list = &cache->slab_list;

retry:
	if (attempt > 1) {
		spinlock_release(&cache->lock);
		return NULL;
	}

	list_for_each_entry(curr, list, slub_list) {
		if (curr->free_count > 0) {
			obj = curr->free_list;
			curr->free_list = *(void **)obj;
			curr->free_count--;
			spinlock_release(&cache->lock);
			return obj;
		}
	}

	if (attempt == 0 && kmem_cache_add_page(cache) < 0) {
		spinlock_release(&cache->lock);
		return NULL;
	}

	attempt++;
	goto retry;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj)
{
	struct page *curr;
	uint64_t start, end;
	struct list_head *list;

	if (!obj)
		return;

	spinlock_acquire(&cache->lock);

	ASSERT(is_aligned((uint64_t)obj, cache->align));

	list = &cache->slab_list;
	list_for_each_entry(curr, list, slub_list) {
		start = page_to_virt(curr);
		end = start + (PAGE_SIZE << curr->cache->page_order);
		if (start <= (uint64_t)obj && (uint64_t)obj < end) {
			*(void **)obj = curr->free_list;
			curr->free_list = obj;
			curr->free_count++;
			spinlock_release(&cache->lock);
			return;
		}
	}

	spinlock_release(&cache->lock);

	panic("%s(): invalid obj\n", __func__);
}
