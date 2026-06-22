#include <brk/assert.h>
#include <brk/list.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/spinlock.h>
#include <brk/string.h>

struct buddy_bucket {
	struct list_head free_list;
};

static struct buddy_bucket areas[PAGE_ORDER_MAX + 1];
static SPINLOCK_DEFINE(areas_lock);

unsigned long page_cache_shrink(unsigned long nr_to_reclaim);

struct page *page_alloc(unsigned int order)
{
	unsigned int curr_order = order;
	struct page *curr = NULL;
	struct page *buddy = NULL;
	bool retried = false;

again:
	spinlock_acquire(&areas_lock);

	for (; curr_order <= PAGE_ORDER_MAX; ++curr_order) {
		if (!list_empty(&areas[curr_order].free_list)) {
			curr = list_last_entry(&areas[curr_order].free_list,
					       struct page, buddy_lru);
			list_del(&curr->buddy_lru);
			goto found;
		}
	}

	spinlock_release(&areas_lock);

	if (!retried) {
		retried = true;
		if (page_cache_shrink(1UL << order) > 0) {
			curr_order = order;
			goto again;
		}
	}
	return NULL;

found:
	/* Split the page until the order matches */
	while (curr->buddy_page_order > order) {
		buddy = curr + ((1ULL << curr->buddy_page_order) >> 1);
		curr->buddy_page_order -= 1;
		buddy->flags = PAGE_FLAGS_NEW_FREE_PAGE;
		buddy->buddy_page_order = curr->buddy_page_order;
		list_add(&buddy->buddy_lru,
			 &areas[buddy->buddy_page_order].free_list);
	}

	curr->flags &= ~PAGE_FLAGS_FREE;
	spinlock_release(&areas_lock);
	return curr;
}

struct page *page_zalloc(unsigned int order)
{
	struct page *pg;
	u64 virt;

	pg = page_alloc(order);
	if (!pg)
		return NULL;
	virt = page_to_virt(pg);
	memset((void *)virt, 0, PAGE_SIZE * (1 << order));
	return pg;
}

static bool page_is_buddy(struct page *pg, struct page *buddy,
			  unsigned int order)
{
	(void)pg;

	if (!(buddy->flags & PAGE_FLAGS_BUDDY))
		return false;

	if (!(buddy->flags & PAGE_FLAGS_HEAD))
		return false;

	if (!(buddy->flags & PAGE_FLAGS_FREE))
		return false;

	if (buddy->buddy_page_order != order)
		return false;

	return true;
}

void page_free(struct page *pg, unsigned int order)
{
	struct page *buddy;

	spinlock_acquire(&areas_lock);

	ASSERT((pg->flags & PAGE_FLAGS_BUDDY));
	ASSERT(!(pg->flags & PAGE_FLAGS_FREE));
	ASSERT((pg->flags & PAGE_FLAGS_HEAD));
	ASSERT(is_aligned(page_to_phys(pg), (1ULL << (PAGE_SHIFT + order))));

	pg->flags = 0;

	while (order < PAGE_ORDER_MAX) {
		buddy = find_buddy_page(pg, order);
		if (!page_is_buddy(pg, buddy, order))
			break;
		list_del(&buddy->buddy_lru);
		buddy->flags = 0;
		pg = min(pg, buddy);
		++order;
	}

	pg->flags = PAGE_FLAGS_NEW_FREE_PAGE;
	pg->buddy_page_order = order;
	list_add(&pg->buddy_lru, &areas[order].free_list);

	spinlock_release(&areas_lock);
}

void page_alloc_init(void)
{
	for (int i = 0; i <= PAGE_ORDER_MAX; ++i)
		list_init(&areas[i].free_list);
}
