#ifndef BRK_PGALLOC_H
#define BRK_PGALLOC_H

#include <arch/mm.h>
#include <arch/page.h>
#include <arch/vas_layout.h>
#include <brk/mm/mm_types.h>

#define PAGE_ORDER_MAX 12

#define PAGE_FLAGS_BUDDY (1 << 0) /* Is this page in the buddy system? */
#define PAGE_FLAGS_HEAD (1 << 1) /* Is this page the head of a block? */
#define PAGE_FLAGS_SLAB (1 << 2) /* Is this page in the SLAB system? */
#define PAGE_FLAGS_FREE (1 << 3) /* Is this page free? */
#define PAGE_FLAGS_KMALLOC (1 << 4) /* Buddy block from kmalloc() > max slab */

#define PAGE_FLAGS_NEW_PAGE (PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD)

#define PAGE_FLAGS_NEW_FREE_PAGE \
	(PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD | PAGE_FLAGS_FREE)

static inline size_t phys_to_pfn(uint64_t paddr)
{
	return (paddr - phys_ram_base) >> PAGE_SHIFT;
}

static inline uint64_t pfn_to_phys(size_t pfn)
{
	return (pfn << PAGE_SHIFT) + phys_ram_base;
}

static inline size_t page_to_pfn(struct page *pg)
{
	return pg - (struct page *)VMEMMAP_START;
}

static inline struct page *pfn_to_page(size_t pfn)
{
	return (struct page *)VMEMMAP_START + pfn;
}

static inline uint64_t page_to_phys(struct page *pg)
{
	return pfn_to_phys(page_to_pfn(pg));
}

static inline struct page *phys_to_page(uint64_t paddr)
{
	return pfn_to_page(phys_to_pfn(paddr));
}

static inline uint64_t page_to_virt(struct page *pg)
{
	return phys_to_virt(page_to_phys(pg));
}

static inline struct page *virt_to_page(uint64_t vaddr)
{
	return phys_to_page(virt_to_phys(vaddr));
}

static inline size_t find_buddy_pfn(size_t pfn, unsigned int order)
{
	return pfn ^ (1 << order);
}

static inline struct page *find_buddy_page(struct page *pg, unsigned int order)
{
	return pfn_to_page(find_buddy_pfn(page_to_pfn(pg), order));
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

#endif
