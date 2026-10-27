#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/list.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/spinlock.h>
#include <brk/string.h>

#define PCP_MAX_ORDER0 64

struct buddy_bucket {
	struct list_head free_list;
};

struct pcp_cache {
	struct list_head pages;
	unsigned int count;
	spinlock_t lock;
};

static struct buddy_bucket areas[PAGE_ORDER_MAX + 1];
static SPINLOCK_DEFINE(areas_lock);
static struct pcp_cache pcps[MAX_CPUS];

unsigned long page_cache_shrink(unsigned long nr_to_reclaim);

static bool page_is_buddy(struct page *pg, struct page *buddy,
			  unsigned int order);

static struct page *pcp_alloc(void)
{
	struct pcp_cache *pcp = &pcps[this_cpuid()];
	struct page *pg = NULL;
	irqstate_t flags = spin_lock_irqsave(&pcp->lock);

	if (!list_empty(&pcp->pages)) {
		pg = list_first_entry(&pcp->pages, struct page, buddy_lru);
		list_del(&pg->buddy_lru);
		pcp->count -= 1;

		ASSERT(pg->flags & PAGE_FLAGS_PCP);
		pg->flags &= ~(PAGE_FLAGS_FREE | PAGE_FLAGS_PCP);
		pg->buddy_page_order = 0;
	}

	spin_unlock_irqrestore(&pcp->lock, flags);
	return pg;
}

static bool pcp_free(struct page *pg)
{
	struct pcp_cache *pcp = &pcps[this_cpuid()];
	bool cached = false;
	irqstate_t flags = spin_lock_irqsave(&pcp->lock);

	if (pcp->count < PCP_MAX_ORDER0) {
		pg->flags = PAGE_FLAGS_NEW_FREE_PAGE | PAGE_FLAGS_PCP;
		pg->buddy_page_order = 0;
		list_add(&pg->buddy_lru, &pcp->pages);
		pcp->count += 1;
		cached = true;
	}

	spin_unlock_irqrestore(&pcp->lock, flags);
	return cached;
}

static void __page_free_to_buddy(struct page *pg, unsigned int order)
{
	struct page *buddy;

	spin_lock(&areas_lock);

	ASSERT((pg->flags & PAGE_FLAGS_BUDDY));
	ASSERT(!(pg->flags & PAGE_FLAGS_FREE));
	ASSERT(!(pg->flags & PAGE_FLAGS_PCP));
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

	spin_unlock(&areas_lock);
}

/*
 * The buddy allocator is out of pages. Push per-CPU caches back into it;
 * any of those pages can satisfy the request. This is the slow path.
 */
static void pcp_drain_all(void)
{
	for (int i = 0; i < MAX_CPUS; ++i) {
		struct pcp_cache *pcp = &pcps[i];
		irqstate_t flags = spin_lock_irqsave(&pcp->lock);

		while (!list_empty(&pcp->pages)) {
			struct page *pg = list_first_entry(
				&pcp->pages, struct page, buddy_lru);

			list_del(&pg->buddy_lru);
			pcp->count -= 1;

			ASSERT(pg->flags & PAGE_FLAGS_PCP);
			pg->flags = PAGE_FLAGS_NEW_PAGE;
			pg->buddy_page_order = 0;
			__page_free_to_buddy(pg, 0);
		}

		spin_unlock_irqrestore(&pcp->lock, flags);
	}
}

struct page *page_alloc(unsigned int order)
{
	unsigned int curr_order = order;
	struct page *curr = NULL;
	struct page *buddy = NULL;
	bool retried = false;

again:
	if (order == 0) {
		curr = pcp_alloc();
		if (curr)
			return curr;
	}

	spin_lock(&areas_lock);

	for (; curr_order <= PAGE_ORDER_MAX; ++curr_order) {
		if (!list_empty(&areas[curr_order].free_list)) {
			curr = list_last_entry(&areas[curr_order].free_list,
					       struct page, buddy_lru);
			list_del(&curr->buddy_lru);
			goto found;
		}
	}

	spin_unlock(&areas_lock);

	if (!retried) {
		retried = true;
		pcp_drain_all();
		page_cache_shrink(1UL << order);
		curr_order = order;
		goto again;
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
	spin_unlock(&areas_lock);
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
	memset((void *)(uintptr_t)virt, 0, PAGE_SIZE * (1 << order));
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

	/* A page sitting in a PCP list has a valid-looking buddy state but
	 * is not linked into the buddy free lists. */
	if (buddy->flags & PAGE_FLAGS_PCP)
		return false;

	if (buddy->buddy_page_order != order)
		return false;

	return true;
}

void page_free(struct page *pg, unsigned int order)
{
	ASSERT((pg->flags & PAGE_FLAGS_BUDDY));
	ASSERT(!(pg->flags & PAGE_FLAGS_FREE));
	ASSERT((pg->flags & PAGE_FLAGS_HEAD));
	ASSERT(is_aligned(page_to_phys(pg), (1ULL << (PAGE_SHIFT + order))));

	if (order == 0 && pcp_free(pg))
		return;

	__page_free_to_buddy(pg, order);
}

void page_alloc_init(void)
{
	for (int i = 0; i <= PAGE_ORDER_MAX; ++i)
		list_init(&areas[i].free_list);

	for (int i = 0; i < MAX_CPUS; ++i) {
		list_init(&pcps[i].pages);
		pcps[i].count = 0;
		spinlock_init(&pcps[i].lock, "pcp");
	}
}
