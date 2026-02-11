#ifndef AOSD_PGALLOC_H
#define AOSD_PGALLOC_H

#include <aosd/asm.h>
#include <aosd/mm.h>
#include <aosd/mm_types.h>

#define PAGE_ORDER_MAX 12

#define PAGE_FLAGS_BUDDY (1 << 0) /* Is this page in the buddy system? */
#define PAGE_FLAGS_HEAD (1 << 1) /* Is this page the head of a block? */
#define PAGE_FLAGS_SLUB (1 << 2) /* Is this page in the SLUB system? */
#define PAGE_FLAGS_FREE (1 << 3) /* Is this page free? */

#define PAGE_FLAGS_NEW_PAGE (PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD)

#define PAGE_FLAGS_NEW_FREE_PAGE \
	(PAGE_FLAGS_BUDDY | PAGE_FLAGS_HEAD | PAGE_FLAGS_FREE)

#define mark_page_busy(p) bitflags_clear((p)->flags, PAGE_FLAGS_FREE)
#define mark_page_free(p) bitflags_set((p)->flags, PAGE_FLAGS_FREE)

void page_alloc_init(void);
struct page *page_alloc(unsigned int order);
void page_free(struct page *page, unsigned int order);

static inline size_t phys_to_pfn(uint64_t paddr)
{
	return (paddr - kernel_map.phys_offset) >> PAGE_SHIFT;
}

static inline uint64_t pfn_to_phys(size_t pfn)
{
	return (pfn << PAGE_SHIFT) + kernel_map.phys_offset;
}

static inline size_t page_to_pfn(struct page *page)
{
	return page - (struct page *)MEM_MAP_START;
}

static inline struct page *pfn_to_page(size_t pfn)
{
	return (struct page *)MEM_MAP_START + pfn;
}

static inline uint64_t page_to_phys(struct page *page)
{
	return pfn_to_phys(page_to_pfn(page));
}

static inline struct page *phys_to_page(uint64_t paddr)
{
	return pfn_to_page(phys_to_pfn(paddr));
}

static inline uint64_t page_to_virt(struct page *page)
{
	return phys_to_virt(page_to_phys(page));
}

static inline struct page *virt_to_page(uint64_t vaddr)
{
	return phys_to_page(virt_to_phys(vaddr));
}

static inline size_t find_buddy_pfn(size_t pfn, unsigned int order)
{
	return pfn ^ (1 << order);
}

static inline struct page *find_buddy_page(struct page *page,
					   unsigned int order)
{
	return pfn_to_page(find_buddy_pfn(page_to_pfn(page), order));
}

static inline unsigned int page_order(size_t size)
{
	size >>= PAGE_SHIFT;
	if (size == 0)
		return 0;
	unsigned int order = 0;
	while (size > 0) {
		++order;
		size >>= 1;
	}
	return order;
}

#endif
