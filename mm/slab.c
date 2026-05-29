#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

static struct kobj_pool kmalloc_caches[NR_KMALLOC_CACHES];

static void slab_mark_page_range(struct page *head, unsigned int order,
				 struct kobj_pool *pool)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i) {
		struct page *p = head + i;

		p->flags |= PAGE_FLAGS_SLAB;
		p->slab_cache = pool;
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
	usize_t pfn = page_to_pfn(pg);
	usize_t head_pfn = pfn & ~(((usize_t)1 << order) - 1);

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

static int kmalloc_obj_pool_index(usize_t size)
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
	usize_t sz[NR_KMALLOC_CACHES] = {
		8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
	};
	usize_t align = alignof(max_align_t);
	for (int i = 0; i < NR_KMALLOC_CACHES; ++i)
		kobj_pool_init(&kmalloc_caches[i], sz[i], align, "kmalloc");
}

void *kmalloc(usize_t size)
{
	if (size == 0)
		return NULL;

	int index = kmalloc_obj_pool_index(size);
	if (index >= 0)
		return kobj_pool_alloc(&kmalloc_caches[index]);

	unsigned int order = page_order(size);
	struct page *pg = page_alloc(order);

	if (!pg)
		return NULL;

	if (order > 0)
		kmalloc_mark_block(pg, order);

	return (void *)page_to_virt(pg);
}

void *kcalloc(usize_t nmemb, usize_t size)
{
	usize_t bytes;

	if (nmemb != 0 && size > SIZE_MAX / nmemb)
		return NULL;

	bytes = nmemb * size;
	void *ptr = kmalloc(bytes);

	if (!ptr)
		return NULL;

	memset(ptr, 0, bytes);
	return ptr;
}

void *kzalloc(usize_t size)
{
	return kcalloc(1, size);
}

void kfree(void *ptr)
{
	if (!ptr)
		return;
	struct page *pg = virt_to_page((u64)ptr);

	if (pg->flags & PAGE_FLAGS_SLAB) {
		kobj_pool_free(pg->slab_cache, ptr);
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

static int kobj_pool_add_page(struct kobj_pool *pool)
{
	usize_t align = pool->align;
	usize_t size = pool->size;

	struct page *pg = page_alloc(pool->page_order);

	if (!pg)
		return -ENOMEM;

	slab_mark_page_range(pg, pool->page_order, pool);

	u64 addr = page_to_virt(pg);
	u64 end_addr = addr + (1 << (PAGE_SHIFT + pool->page_order));

	if (!is_aligned(addr, align))
		addr = round_up(addr, align);

	pg->slab_free_objs = (void *)addr;

	pg->slab_objs_count = pg->slab_free_count = (end_addr - addr) / size;
	if (pg->slab_objs_count == 0) {
		slab_unmark_page_range(pg, pool->page_order);
		page_free(pg, pool->page_order);
		return -ENOMEM;
	}

	while (addr + size < end_addr) {
		void **curr_next_ptr = (void **)addr;
		void *next = (void *)(addr + size);

		*curr_next_ptr = next;
		addr += size;
	}
	*(void **)addr = NULL;

	list_add(&pg->slab_list, &pool->slab_list);

	return 0;
}

int kobj_pool_init(struct kobj_pool *pool, usize_t size, usize_t align,
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

	pool->size = size;
	pool->align = align;
	pool->page_order = page_order(size);
	pool->name = name;
	spinlock_init(&pool->lock, name);

	list_init(&pool->slab_list);

	spinlock_acquire(&pool->lock);
	ret = kobj_pool_add_page(pool);
	spinlock_release(&pool->lock);
	return ret;
}

void kobj_pool_deinit(struct kobj_pool *pool)
{
	struct list_head *first;
	struct page *pg;
	struct list_head *list;

	spinlock_acquire(&pool->lock);
	list = &pool->slab_list;
	while (!list_empty(list)) {
		first = list->next;
		list_del(first);
		pg = list_entry(first, struct page, slab_list);
		ASSERT(pg->slab_free_count == pg->slab_objs_count);
		slab_unmark_page_range(pg, pool->page_order);
		ASSERT(pg);
		page_free(pg, pool->page_order);
	}
	spinlock_release(&pool->lock);
}

void *kobj_pool_alloc(struct kobj_pool *pool)
{
	struct page *curr;
	void *obj;
	int attempt = 0;
	struct list_head *list;

	spinlock_acquire(&pool->lock);
	list = &pool->slab_list;

retry:
	if (attempt > 1) {
		spinlock_release(&pool->lock);
		return NULL;
	}

	list_for_each_entry(curr, list, slab_list) {
		if (curr->slab_free_count > 0) {
			obj = curr->slab_free_objs;
			curr->slab_free_objs = *(void **)obj;
			curr->slab_free_count--;
			spinlock_release(&pool->lock);
			return obj;
		}
	}

	if (attempt == 0 && kobj_pool_add_page(pool) < 0) {
		spinlock_release(&pool->lock);
		return NULL;
	}

	attempt++;
	goto retry;
}

void kobj_pool_free(struct kobj_pool *pool, void *obj)
{
	struct page *curr;
	u64 start, end;
	struct list_head *list;

	if (!obj)
		return;

	spinlock_acquire(&pool->lock);

	ASSERT(is_aligned((u64)obj, pool->align));

	list = &pool->slab_list;
	list_for_each_entry(curr, list, slab_list) {
		start = page_to_virt(curr);
		end = start + (PAGE_SIZE << curr->slab_cache->page_order);
		if (start <= (u64)obj && (u64)obj < end) {
			*(void **)obj = curr->slab_free_objs;
			curr->slab_free_objs = obj;
			curr->slab_free_count++;
			spinlock_release(&pool->lock);
			return;
		}
	}

	spinlock_release(&pool->lock);

	panic("%s(): invalid obj %p in cache %s\n", __func__, obj,
	      pool->name ? pool->name : "(null)");
}
