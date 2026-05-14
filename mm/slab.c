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
#include <brk/types.h>

static struct kmem_cache kmalloc_caches[NR_KMALLOC_CACHES];

static void slab_mark_page_range(struct page *head, unsigned int order,
				 struct kmem_cache *cache)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i) {
		struct page *p = head + i;

		p->flags |= PAGE_FLAGS_SLAB;
		p->slab_cache = cache;
	}
}

static void slab_unmark_page_range(struct page *head, unsigned int order)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i)
		(head + i)->flags &= ~PAGE_FLAGS_SLAB;
}

static struct page *kmalloc_block_head(struct page *pg, unsigned int order)
{
	size_t pfn = page_to_pfn(pg);
	size_t head_pfn = pfn & ~(((size_t)1 << order) - 1);

	return pfn_to_page(head_pfn);
}

static void kmalloc_mark_block(struct page *head, unsigned int order)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i) {
		struct page *p = head + i;

		p->flags |= PAGE_FLAGS_KMALLOC;
		p->buddy_page_order = order;
	}
}

static void kmalloc_unmark_block(struct page *head, unsigned int order)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i)
		(head + i)->flags &= ~PAGE_FLAGS_KMALLOC;
}

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

	unsigned int order = page_order(size);
	struct page *pg = page_alloc(order);

	if (!pg)
		return NULL;

	if (order > 0)
		kmalloc_mark_block(pg, order);

	return (void *)page_to_virt(pg);
}

void *kcalloc(size_t nmemb, size_t size)
{
	size_t bytes;

	if (nmemb != 0 && size > SIZE_MAX / nmemb)
		return NULL;

	bytes = nmemb * size;
	void *ptr = kmalloc(bytes);

	if (!ptr)
		return NULL;

	memset(ptr, 0, bytes);
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

	if (pg->flags & PAGE_FLAGS_SLAB) {
		kmem_cache_free(pg->slab_cache, ptr);
	} else if (pg->flags & PAGE_FLAGS_KMALLOC) {
		struct page *head =
			kmalloc_block_head(pg, pg->buddy_page_order);

		ASSERT(head->flags & PAGE_FLAGS_HEAD);
		kmalloc_unmark_block(head, head->buddy_page_order);
		page_free(head, head->buddy_page_order);
	} else {
		ASSERT(pg);
		page_free(pg, pg->buddy_page_order);
	}
}

static int kmem_cache_add_page(struct kmem_cache *cache)
{
	size_t align = cache->align;
	size_t size = cache->size;

	struct page *pg = page_alloc(cache->page_order);

	if (!pg)
		return -ENOMEM;

	slab_mark_page_range(pg, cache->page_order, cache);

	uint64_t addr = page_to_virt(pg);
	uint64_t end_addr = addr + (1 << (PAGE_SHIFT + cache->page_order));

	if (!is_aligned(addr, align))
		addr = round_up(addr, align);

	pg->slab_free_objs = (void *)addr;

	pg->slab_objs_count = pg->slab_free_count = (end_addr - addr) / size;
	if (pg->slab_objs_count == 0) {
		slab_unmark_page_range(pg, cache->page_order);
		page_free(pg, cache->page_order);
		return -ENOMEM;
	}

	while (addr + size < end_addr) {
		void **curr_next_ptr = (void **)addr;
		void *next = (void *)(addr + size);

		*curr_next_ptr = next;
		addr += size;
	}
	*(void **)addr = NULL;

	list_add(&pg->slab_list, &cache->slab_list);

	return 0;
}

int kmem_cache_init(struct kmem_cache *cache, size_t size, size_t align,
		    const char *name)
{
	int ret;

	if (size == 0 || align == 0)
		return -EINVAL;

	if (align < sizeof(void *))
		align = sizeof(void *);

	if (size < sizeof(void *))
		size = sizeof(void *);

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
		pg = list_entry(first, struct page, slab_list);
		ASSERT(pg->slab_free_count == pg->slab_objs_count);
		slab_unmark_page_range(pg, cache->page_order);
		ASSERT(pg);
		page_free(pg, cache->page_order);
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

	list_for_each_entry(curr, list, slab_list) {
		if (curr->slab_free_count > 0) {
			obj = curr->slab_free_objs;
			curr->slab_free_objs = *(void **)obj;
			curr->slab_free_count--;
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
	list_for_each_entry(curr, list, slab_list) {
		start = page_to_virt(curr);
		end = start + (PAGE_SIZE << curr->slab_cache->page_order);
		if (start <= (uint64_t)obj && (uint64_t)obj < end) {
			*(void **)obj = curr->slab_free_objs;
			curr->slab_free_objs = obj;
			curr->slab_free_count++;
			spinlock_release(&cache->lock);
			return;
		}
	}

	spinlock_release(&cache->lock);

	panic("%s(): invalid obj %p in cache %s\n", __func__, obj,
	      cache->name ? cache->name : "(null)");
}
