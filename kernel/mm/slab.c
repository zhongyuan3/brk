#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/brk/errno.h>

static void slab_mark_page_range(struct page *head, unsigned int order,
				 struct slab_allocator *allocator)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i) {
		struct page *p = head + i;

		p->flags |= PAGE_FLAGS_SLAB;
		p->slab_cache = allocator;
	}
}

static void slab_unmark_page_range(struct page *head, unsigned int order)
{
	unsigned int nr = 1U << order;

	for (unsigned int i = 0; i < nr; ++i)
		(head + i)->flags &= ~PAGE_FLAGS_SLAB;
}

static int slab_add_page(struct slab_allocator *allocator)
{
	size_t align = allocator->align;
	size_t size = allocator->size;

	struct page *pg = page_alloc(allocator->page_order);

	if (!pg)
		return -ENOMEM;

	slab_mark_page_range(pg, allocator->page_order, allocator);

	uint64_t addr = page_to_virt(pg);
	uint64_t end_addr = addr + (1 << (PAGE_SHIFT + allocator->page_order));

	if (!is_aligned(addr, align))
		addr = round_up(addr, align);

	pg->slab_free_objs = (void *)addr;

	pg->slab_objs_count = pg->slab_free_count = (end_addr - addr) / size;
	if (pg->slab_objs_count == 0) {
		slab_unmark_page_range(pg, allocator->page_order);
		page_free(pg, allocator->page_order);
		return -ENOMEM;
	}

	while (addr + size < end_addr) {
		void **curr_next_ptr = (void **)addr;
		void *next = (void *)(addr + size);

		*curr_next_ptr = next;
		addr += size;
	}
	*(void **)addr = NULL;

	list_add(&pg->slab_list, &allocator->slab_list);

	return 0;
}

int slab_init(struct slab_allocator *allocator, size_t size, size_t align,
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

	allocator->size = size;
	allocator->align = align;
	allocator->page_order = page_order(size);
	allocator->name = name;
	spinlock_init(&allocator->lock, name);

	list_init(&allocator->slab_list);

	spinlock_acquire(&allocator->lock);
	ret = slab_add_page(allocator);
	spinlock_release(&allocator->lock);
	return ret;
}

void slab_deinit(struct slab_allocator *allocator)
{
	struct list_head *first;
	struct page *pg;
	struct list_head *list;

	spinlock_acquire(&allocator->lock);
	list = &allocator->slab_list;
	while (!list_empty(list)) {
		first = list->next;
		list_del(first);
		pg = list_entry(first, struct page, slab_list);
		ASSERT(pg->slab_free_count == pg->slab_objs_count);
		slab_unmark_page_range(pg, allocator->page_order);
		ASSERT(pg);
		page_free(pg, allocator->page_order);
	}
	spinlock_release(&allocator->lock);
}

void *slab_alloc(struct slab_allocator *allocator)
{
	struct page *curr;
	void *obj;
	int attempt = 0;
	struct list_head *list;

	spinlock_acquire(&allocator->lock);
	list = &allocator->slab_list;

retry:
	if (attempt > 1) {
		spinlock_release(&allocator->lock);
		return NULL;
	}

	list_for_each_entry(curr, list, slab_list) {
		if (curr->slab_free_count > 0) {
			obj = curr->slab_free_objs;
			curr->slab_free_objs = *(void **)obj;
			curr->slab_free_count--;
			spinlock_release(&allocator->lock);
			return obj;
		}
	}

	if (attempt == 0 && slab_add_page(allocator) < 0) {
		spinlock_release(&allocator->lock);
		return NULL;
	}

	attempt++;
	goto retry;
}

void *slab_alloc_zero(struct slab_allocator *allocator)
{
	void *obj = slab_alloc(allocator);
	if (!obj)
		return NULL;
	memset(obj, 0, allocator->size);
	return obj;
}

void slab_free(struct slab_allocator *allocator, void *obj)
{
	struct page *curr;
	uint64_t start, end;
	struct list_head *list;

	if (!obj)
		return;

	spinlock_acquire(&allocator->lock);

	ASSERT(is_aligned((uint64_t)obj, allocator->align));

	list = &allocator->slab_list;
	list_for_each_entry(curr, list, slab_list) {
		start = page_to_virt(curr);
		end = start + (PAGE_SIZE << curr->slab_cache->page_order);
		if (start <= (uint64_t)obj && (uint64_t)obj < end) {
			*(void **)obj = curr->slab_free_objs;
			curr->slab_free_objs = obj;
			curr->slab_free_count++;
			spinlock_release(&allocator->lock);
			return;
		}
	}

	spinlock_release(&allocator->lock);

	panic("%s(): invalid obj %p in cache %s\n", __func__, obj,
	      allocator->name ? allocator->name : "(null)");
}
