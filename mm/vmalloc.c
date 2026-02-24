#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/assert.h>
#include <aosd/errno.h>
#include <aosd/list.h>
#include <aosd/lock.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/slab.h>
#include <aosd/spinlock.h>
#include <aosd/string.h>
#include <aosd/vmalloc.h>

static struct kmem_cache vma_cache;
static struct list_head vma;
static spinlock_define(vma_lock);

static void vunmap_range(pgde_t *pgd, uint64_t addr, uint64_t end_addr,
			 struct vmap_ops *ops)
{
	size_t rem_size;
	pgde_t *pgdep;
	pmde_t *pmd, *pmdep;
	pte_t *pt, *ptep;

	while (addr < end_addr) {
		rem_size = end_addr - addr;
		pgdep = pgd + pgde_index(addr);
		if (pgde_large(*pgdep)) {
			assert(rem_size >= PAGE_SIZE_1G);
			pgde_clear(pgdep);
			addr += PAGE_SIZE_1G;
			continue;
		}
		assert(pgde_present(*pgdep));
		pmd = ops->get_pmd_virt(pgde_get_pmd(*pgdep));
		pmdep = pmd + pmde_index(addr);
		if (pmde_large(*pmdep)) {
			assert(rem_size >= PAGE_SIZE_2M);
			pmde_clear(pmdep);
			addr += PAGE_SIZE_2M;
			continue;
		}
		assert(pmde_present(*pmdep));
		pt = ops->get_pt_virt(pmde_get_pt(*pmdep));
		ptep = pt + pte_index(addr);
		assert(pte_present(*ptep));
		pte_clear(ptep);
		addr += PAGE_SIZE;
	}
}

static int vmap_range(pgde_t *pgd, uint64_t addr, uint64_t end_addr,
		      uint64_t paddr, size_t page_size, unsigned int flags,
		      struct vmap_ops *ops)
{
	pgde_t *pgdep;
	pmde_t *pmd, *pmdep;
	pte_t *pt, *ptep;
	uint64_t pmd_phys, pt_phys;
	uint64_t start_addr = addr;

	for (; addr < end_addr; addr += page_size, paddr += page_size) {
		pgdep = pgd + pgde_index(addr);
		if (page_size == PAGE_SIZE_1G) {
			assert(!pgde_present(*pgdep));
			pgde_set_large(pgdep, paddr, flags);
			continue;
		}
		if (!pgde_present(*pgdep)) {
			pmd_phys = ops->alloc_pmd();
			if (!pmd_phys) {
				vunmap_range(pgd, start_addr, addr, ops);
				return -ENOMEM;
			}
			pmd = ops->get_pmd_virt(pmd_phys);
			pgde_set_pmd(pgdep, pmd_phys);
		} else {
			pmd = ops->get_pmd_virt(pgde_get_pmd(*pgdep));
		}
		pmdep = pmd + pmde_index(addr);
		if (page_size == PAGE_SIZE_2M) {
			assert(!pmde_present(*pmdep));
			pmde_set_large(pmdep, paddr, flags);
			continue;
		}
		if (!pmde_present(*pmdep)) {
			pt_phys = ops->alloc_pt();
			if (!pt_phys) {
				vunmap_range(pgd, start_addr, addr, ops);
				return -ENOMEM;
			}
			pt = ops->get_pt_virt(pt_phys);
			pmde_set_pt(pmdep, pt_phys);
		} else {
			pt = ops->get_pt_virt(pmde_get_pt(*pmdep));
		}
		ptep = pt + pte_index(addr);
		assert(!pte_present(*ptep));
		pte_set(ptep, paddr, flags);
	}

	return 0;
}

int vmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	 unsigned int flags, struct vmap_ops *ops)
{
	uint64_t end_addr;
	size_t rem_size;
	size_t page_size;
	int err;

	assert(is_aligned(addr, PAGE_SIZE));
	assert(is_aligned(size, PAGE_SIZE));
	assert(is_aligned(paddr, PAGE_SIZE));

	end_addr = addr + size;
	while (addr < end_addr) {
		rem_size = end_addr - addr;

		if (is_aligned(addr, PAGE_SIZE_1G) &&
		    is_aligned(paddr, PAGE_SIZE_1G) &&
		    rem_size >= PAGE_SIZE_1G) {
			page_size = PAGE_SIZE_1G;
			rem_size = align_down(rem_size, PAGE_SIZE_1G);
		} else if (is_aligned(addr, PAGE_SIZE_2M) &&
			   is_aligned(paddr, PAGE_SIZE_2M) &&
			   rem_size >= PAGE_SIZE_2M) {
			page_size = PAGE_SIZE_2M;
			rem_size = align_down(rem_size, PAGE_SIZE_2M);
		} else {
			page_size = PAGE_SIZE;
			rem_size = align_down(rem_size, PAGE_SIZE);
		}

		err = vmap_range(pgd, addr, addr + rem_size, paddr, page_size,
				 flags, ops);
		if (err)
			return err;

		addr += rem_size;
		paddr += rem_size;
	}

	return 0;
}

void vunmap(pgde_t *pgd, uint64_t addr, size_t size, struct vmap_ops *ops)
{
	assert(is_aligned(addr, PAGE_SIZE));
	assert(is_aligned(size, PAGE_SIZE));
	vunmap_range(pgd, addr, addr + size, ops);
}

static uint64_t alloc_pgtable(void)
{
	struct page *page = page_alloc(0);
	if (!page)
		return 0;
	uint64_t paddr = page_to_phys(page);
	memset((void *)phys_to_virt(paddr), 0, PAGE_SIZE);
	return paddr;
}

static uint64_t alloc_pgd(void)
{
	return alloc_pgtable();
}

static uint64_t alloc_pmd(void)
{
	return alloc_pgtable();
}

static uint64_t alloc_pt(void)
{
	return alloc_pgtable();
}

static pgde_t *get_pgd_virt(uint64_t pgd_phys)
{
	return (pgde_t *)phys_to_virt(pgd_phys);
}

static pmde_t *get_pmd_virt(uint64_t pmd_phys)
{
	return (pmde_t *)phys_to_virt(pmd_phys);
}

static pte_t *get_pt_virt(uint64_t pt_phys)
{
	return (pte_t *)phys_to_virt(pt_phys);
}

int kvmap(uint64_t addr, size_t size, uint64_t paddr, unsigned int flags)
{
	int ret;
	struct vmap_ops ops = {
		.alloc_pgd = alloc_pgd,
		.alloc_pmd = alloc_pmd,
		.alloc_pt = alloc_pt,
		.get_pgd_virt = get_pgd_virt,
		.get_pmd_virt = get_pmd_virt,
		.get_pt_virt = get_pt_virt,
	};
	spinlock_acquire(&kernel_pgdir_lock);
	ret = vmap(kernel_pgdir, addr, size, paddr, flags, &ops);
	spinlock_release(&kernel_pgdir_lock);
	return ret;
}

void kvunmap(uint64_t addr, size_t size)
{
	struct vmap_ops ops = {
		.alloc_pgd = alloc_pgd,
		.alloc_pmd = alloc_pmd,
		.alloc_pt = alloc_pt,
		.get_pgd_virt = get_pgd_virt,
		.get_pmd_virt = get_pmd_virt,
		.get_pt_virt = get_pt_virt,
	};
	spinlock_acquire(&kernel_pgdir_lock);
	vunmap(kernel_pgdir, addr, size, &ops);
	spinlock_release(&kernel_pgdir_lock);
}

int uvmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	  unsigned int flags)
{
	struct vmap_ops ops = {
		.alloc_pgd = alloc_pgd,
		.alloc_pmd = alloc_pmd,
		.alloc_pt = alloc_pt,
		.get_pgd_virt = get_pgd_virt,
		.get_pmd_virt = get_pmd_virt,
		.get_pt_virt = get_pt_virt,
	};
	return vmap(pgd, addr, size, paddr, flags | PTE_U, &ops);
}

void uvunmap(pgde_t *pgd, uint64_t addr, size_t size)
{
	struct vmap_ops ops = {
		.alloc_pgd = alloc_pgd,
		.alloc_pmd = alloc_pmd,
		.alloc_pt = alloc_pt,
		.get_pgd_virt = get_pgd_virt,
		.get_pmd_virt = get_pmd_virt,
		.get_pt_virt = get_pt_virt,
	};
	vunmap(pgd, addr, size, &ops);
}

static struct vmem_area *find_vmem_area(uint64_t addr)
{
	struct vmem_area *area;

	list_for_each_entry(area, &vma, list)
		if (area->addr == addr)
			return area;

	return NULL;
}

static void merge_free_vmem_areas(void)
{
	struct vmem_area *curr, *next;

	list_for_each_entry_safe(curr, next, &vma, list) {
		if (curr->is_free && next->is_free &&
		    curr->addr + curr->size == next->addr) {
			next->addr = curr->addr;
			next->size += curr->size;
			list_del(&curr->list);
			kmem_cache_free(&vma_cache, curr);
		}
	}
}

static struct vmem_area *get_free_vmem_area(size_t size)
{
	struct vmem_area *area;
	struct vmem_area *new_area;

	size = align_up(size, PAGE_SIZE);

	list_for_each_entry(area, &vma, list) {
		if (!area->is_free)
			continue;
		if (area->size < size) {
			continue;
		} else if (area->size == size) {
			area->is_free = false;
			return area;
		} else {
			new_area = kmem_cache_alloc(&vma_cache);
			if (!new_area)
				return NULL;
			new_area->addr = area->addr + size;
			new_area->size = area->size - size;
			new_area->is_free = true;
			area->size = size;
			area->is_free = false;
			list_add(&new_area->list, &area->list);
			return area;
		}
	}

	return NULL;
}

static void free_vmem_area(struct vmem_area *area)
{
	area->is_free = true;
	merge_free_vmem_areas();
}

void vmalloc_init(void)
{
	kmem_cache_init(&vma_cache, sizeof(struct vmem_area),
			alignof(struct vmem_area), "vma_cache");
	list_init_head(&vma);
	struct vmem_area *area = kmem_cache_alloc(&vma_cache);
	area->addr = VMALLOC_START;
	area->size = VMALLOC_SIZE;
	area->is_free = true;
	list_add(&area->list, &vma);
}

void *vmalloc(size_t size)
{
	spinlock_acquire(&vma_lock);

	size = align_up(size, PAGE_SIZE);

	struct vmem_area *area = get_free_vmem_area(size);
	if (!area) {
		spinlock_release(&vma_lock);
		return NULL;
	}

	size_t nr_pages = size >> PAGE_SHIFT;

	area->pages = kcalloc(nr_pages, sizeof(struct page *));
	if (!area->pages) {
		free_vmem_area(area);
		spinlock_release(&vma_lock);
		return NULL;
	}
	area->nr_pages = nr_pages;

	size_t i = 0;
	uint64_t vaddr = area->addr;
	for (; i < nr_pages; ++i) {
		struct page *page = page_alloc(0);
		if (!page)
			goto out_cleanup;
		area->pages[i] = page;
		if (kvmap(vaddr, PAGE_SIZE, page_to_phys(page),
			  PTE_R | PTE_W)) {
			page_free(page, 0);
			goto out_cleanup;
		}
		vaddr += PAGE_SIZE;
	}

	spinlock_release(&vma_lock);
	return (void *)area->addr;

out_cleanup:
	for (size_t j = 0; j < i; ++j)
		page_free(area->pages[j], 0);
	kfree(area->pages);
	free_vmem_area(area);
	spinlock_release(&vma_lock);
	return NULL;
}

void vfree(void *ptr)
{
	struct vmem_area *area;

	spinlock_acquire(&vma_lock);
	area = find_vmem_area((uint64_t)ptr);
	if (!area || area->is_free) {
		spinlock_release(&vma_lock);
		return;
	}

	kvunmap(area->addr, area->size);
	for (size_t i = 0; i < area->nr_pages; ++i)
		page_free(area->pages[i], 0);
	kfree(area->pages);
	free_vmem_area(area);
	spinlock_release(&vma_lock);
}

void *vmalloc_nomap(size_t size)
{
	struct vmem_area *area;

	spinlock_acquire(&vma_lock);
	area = get_free_vmem_area(align_up(size, PAGE_SIZE));
	spinlock_release(&vma_lock);
	if (!area)
		return NULL;

	return (void *)area->addr;
}

void vfree_nomap(void *ptr)
{
	struct vmem_area *area;

	spinlock_acquire(&vma_lock);
	area = find_vmem_area((uint64_t)ptr);
	if (!area || area->is_free) {
		spinlock_release(&vma_lock);
		return;
	}
	free_vmem_area(area);
	spinlock_release(&vma_lock);
}
