#ifndef BRK_PGALLOC_H
#define BRK_PGALLOC_H

#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/mm.h>
#include <brk/mm_types.h>
#include <stdbool.h>

#define PAGE_ORDER_MAX 12

typedef unsigned long pfn_t;

#define PFN_DOWN(addr) ((pfn_t)((u64)(addr) >> PAGE_SHIFT))
#define PFN_UP(addr) ((pfn_t)(((u64)(addr) + PAGE_SIZE - 1) >> PAGE_SHIFT))
#define PFN_PHYS(pfn) ((u64)(pfn) << PAGE_SHIFT)

extern pfn_t min_pfn;
extern pfn_t max_pfn;
extern pfn_t vmemmap_start_pfn;

#define PAGE_FLAGS_BUDDY (1 << 0) /* Is this page in the buddy system? */
#define PAGE_FLAGS_HEAD (1 << 1) /* Is this page the head of a block? */
#define PAGE_FLAGS_SLAB (1 << 2) /* Is this page in the SLAB system? */
#define PAGE_FLAGS_FREE (1 << 3) /* Is this page free? */
#define PAGE_FLAGS_KMALLOC (1 << 4) /* Buddy block from kmalloc() > max slab */
#define PAGE_FLAGS_PCP (1 << 5) /* Cached in a per-CPU page list */

#define PAGE_FLAGS_NEW_PAGE (PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD)

#define PAGE_FLAGS_NEW_FREE_PAGE \
	(PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD | PAGE_FLAGS_FREE)

static inline pfn_t phys_to_pfn(u64 paddr)
{
	return PFN_DOWN(paddr);
}

static inline u64 pfn_to_phys(pfn_t pfn)
{
	return PFN_PHYS(pfn);
}

static inline pfn_t page_to_pfn(const struct page *pg)
{
	return (pfn_t)(pg - (const struct page *)VMEMMAP_START) +
	       vmemmap_start_pfn;
}

static inline struct page *pfn_to_page(pfn_t pfn)
{
	return (struct page *)VMEMMAP_START + (pfn - vmemmap_start_pfn);
}

static inline u64 page_to_phys(const struct page *pg)
{
	return pfn_to_phys(page_to_pfn(pg));
}

static inline struct page *phys_to_page(u64 paddr)
{
	return pfn_to_page(phys_to_pfn(paddr));
}

static inline uintptr_t page_to_virt(const struct page *pg)
{
	return phys_to_virt(page_to_phys(pg));
}

static inline struct page *virt_to_page(uintptr_t vaddr)
{
	return phys_to_page(virt_to_phys(vaddr));
}

static inline pfn_t find_buddy_pfn(pfn_t pfn, unsigned int order)
{
	return pfn ^ (1UL << order);
}

bool pfn_valid(pfn_t pfn);

static inline struct page *find_buddy_page(struct page *pg, unsigned int order)
{
	pfn_t buddy_pfn = find_buddy_pfn(page_to_pfn(pg), order);

	if (!pfn_valid(buddy_pfn))
		return NULL;

	return pfn_to_page(buddy_pfn);
}

static inline unsigned int page_order(size_t size)
{
	for (unsigned int order = 0; order < PAGE_ORDER_MAX; ++order)
		if (PAGE_SIZE * (1U << order) >= size)
			return order;

	return PAGE_ORDER_MAX;
}

void page_alloc_init(void);
struct page *page_alloc(unsigned int order);
struct page *page_zalloc(unsigned int order);
void page_free(struct page *pg, unsigned int order);

/* Reference counting for pages that may be shared by COW mappings. */
void page_get(struct page *pg);
void page_put(struct page *pg);

#endif
