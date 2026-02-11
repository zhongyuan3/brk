#include <aosd/assert.h>
#include <aosd/list.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>

static struct free_area areas[PAGE_ORDER_MAX + 1];

struct page *page_alloc(unsigned int order)
{
	unsigned int curr_order = order;
	struct page *curr = NULL;
	struct page *buddy = NULL;

	for (; curr_order <= PAGE_ORDER_MAX; ++curr_order) {
		if (!list_empty(&areas[curr_order].free_list)) {
			curr = list_last_entry(&areas[curr_order].free_list,
					       struct page, lru);
			list_del(&curr->lru);
			goto found;
		}
	}
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

	mark_page_busy(curr);
	return curr;
}

static bool page_is_buddy(struct page *page, struct page *buddy,
			  unsigned int order)
{
	if (!bitflags_check(buddy->flags, PAGE_FLAGS_BUDDY))
		return false;

	if (!bitflags_check(buddy->flags, PAGE_FLAGS_HEAD))
		return false;

	if (!bitflags_check(buddy->flags, PAGE_FLAGS_FREE))
		return false;

	if (buddy->order != order)
		return false;

	return true;
}

void page_free(struct page *page, unsigned int order)
{
	struct page *buddy;

	assert(bitflags_check(page->flags, PAGE_FLAGS_BUDDY));
	assert(!bitflags_check(page->flags, PAGE_FLAGS_FREE));
	assert(bitflags_check(page->flags, PAGE_FLAGS_HEAD));
	assert(is_aligned(page_to_phys(page), (1ULL << (PAGE_SHIFT + order))));

	page->flags = 0;

	while (order < PAGE_ORDER_MAX) {
		buddy = find_buddy_page(page, order);
		if (!page_is_buddy(page, buddy, order))
			break;
		list_del(&buddy->lru);
		buddy->flags = 0;
		page = min(page, buddy);
		++order;
	}

	page->flags = PAGE_FLAGS_NEW_FREE_PAGE;
	page->order = order;
	list_add(&page->lru, &areas[order].free_list);
}

void page_alloc_init(void)
{
	for (int i = 0; i <= PAGE_ORDER_MAX; ++i)
		list_init_head(&areas[i].free_list);
}
