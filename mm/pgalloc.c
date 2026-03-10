#include <aosd/assert.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>

static struct free_area areas[PAGE_ORDER_MAX + 1];
static SPINLOCK_DEFINE(areas_lock);

struct page *page_alloc(unsigned int order)
{
	unsigned int curr_order = order;
	struct page *curr = NULL;
	struct page *buddy = NULL;

	spinlock_acquire(&areas_lock);

	for (; curr_order <= PAGE_ORDER_MAX; ++curr_order) {
		if (!list_empty(&areas[curr_order].free_list)) {
			curr = list_last_entry(&areas[curr_order].free_list,
					       struct page, lru);
			list_del(&curr->lru);
			goto found;
		}
	}

	spinlock_release(&areas_lock);
	return NULL;

found:
	/* Split the page until the order matches */
	while (curr->order > order) {
		buddy = curr + ((1ULL << curr->order) >> 1);
		curr->order -= 1;
		buddy->flags = PAGE_FLAGS_NEW_FREE_PAGE;
		buddy->order = curr->order;
		list_add(&buddy->lru, &areas[buddy->order].free_list);
	}

	curr->flags &= ~PAGE_FLAGS_FREE;
	spinlock_release(&areas_lock);
	return curr;
}

static bool page_is_buddy(struct page *pg, struct page *buddy,
			  unsigned int order)
{
	if (!(buddy->flags & PAGE_FLAGS_BUDDY))
		return false;

	if (!(buddy->flags & PAGE_FLAGS_HEAD))
		return false;

	if (!(buddy->flags & PAGE_FLAGS_FREE))
		return false;

	if (buddy->order != order)
		return false;

	return true;
}

void page_free(struct page *pg, unsigned int order)
{
	struct page *buddy;

	spinlock_acquire(&areas_lock);

	assert((pg->flags & PAGE_FLAGS_BUDDY));
	assert(!(pg->flags & PAGE_FLAGS_FREE));
	assert((pg->flags & PAGE_FLAGS_HEAD));
	assert(is_aligned(page_to_phys(pg), (1ULL << (PAGE_SHIFT + order))));

	pg->flags = 0;

	while (order < PAGE_ORDER_MAX) {
		buddy = find_buddy_page(pg, order);
		if (!page_is_buddy(pg, buddy, order))
			break;
		list_del(&buddy->lru);
		buddy->flags = 0;
		pg = min(pg, buddy);
		++order;
	}

	pg->flags = PAGE_FLAGS_NEW_FREE_PAGE;
	pg->order = order;
	list_add(&pg->lru, &areas[order].free_list);

	spinlock_release(&areas_lock);
}

void page_alloc_init(void)
{
	for (int i = 0; i <= PAGE_ORDER_MAX; ++i)
		list_init(&areas[i].free_list);
}
