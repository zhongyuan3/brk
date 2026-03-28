#include "aosd/memblock.h"
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
#include <aosd/string.h>
#include <aosd/vmalloc.h>

static struct kmem_cache vma_cache;
static struct list_head vma;
static SPINLOCK_DEFINE(vma_lock);

static uint64_t alloc_pgtable(enum vmap_mode mode)
{
	struct page *pg;
	uint64_t paddr;

	switch (mode) {
	case VMAP_MODE_EARLY:
		paddr = memblock_alloc(PAGE_SIZE, _EKERNEL_PHYS, PAGE_SIZE);
		if (!paddr)
			panic("%s(): memblock_alloc() failed\n", __func__);
		memset((void *)paddr, 0, PAGE_SIZE);
		return paddr;
	case VMAP_MODE_INTERIM:
		paddr = memblock_alloc(PAGE_SIZE, _EKERNEL_PHYS, PAGE_SIZE);
		if (!paddr)
			panic("%s(): memblock_alloc() failed\n", __func__);
		memset((void *)phys_to_virt(paddr), 0, PAGE_SIZE);
		return paddr;
	case VMAP_MODE_FINAL:
		pg = page_alloc(0);
		if (!pg)
			return 0;
		paddr = page_to_phys(pg);
		memset((void *)phys_to_virt(paddr), 0, PAGE_SIZE);
		return paddr;
	}
	panic("%s(): unexpected mode: %d\n", __func__, mode);
}

static uint64_t alloc_pmd(enum vmap_mode mode)
{
	return alloc_pgtable(mode);
}

static uint64_t alloc_pt(enum vmap_mode mode)
{
	return alloc_pgtable(mode);
}

static pmde_t *get_pmd_virt(uint64_t pmd_phys, enum vmap_mode mode)
{
	switch (mode) {
	case VMAP_MODE_EARLY:
		return (pmde_t *)pmd_phys;
	case VMAP_MODE_INTERIM:
		return (pmde_t *)phys_to_virt(pmd_phys);
	case VMAP_MODE_FINAL:
		return (pmde_t *)phys_to_virt(pmd_phys);
	}
	panic("%s(): unexpected mode: %d\n", __func__, mode);
}

static pte_t *get_pt_virt(uint64_t pt_phys, enum vmap_mode mode)
{
	switch (mode) {
	case VMAP_MODE_EARLY:
		return (pte_t *)pt_phys;
	case VMAP_MODE_INTERIM:
		return (pte_t *)phys_to_virt(pt_phys);
	case VMAP_MODE_FINAL:
		return (pte_t *)phys_to_virt(pt_phys);
	}
	panic("%s(): unexpected mode: %d\n", __func__, mode);
}

static void vunmap_range(pgde_t *pgd, uint64_t addr, uint64_t end_addr,
			 enum vmap_mode mode)
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
		pmd = get_pmd_virt(pgde_get_pmd(*pgdep), mode);
		pmdep = pmd + pmde_index(addr);
		if (pmde_large(*pmdep)) {
			assert(rem_size >= PAGE_SIZE_2M);
			pmde_clear(pmdep);
			addr += PAGE_SIZE_2M;
			continue;
		}
		assert(pmde_present(*pmdep));
		pt = get_pt_virt(pmde_get_pt(*pmdep), mode);
		ptep = pt + pte_index(addr);
		assert(pte_present(*ptep));
		pte_clear(ptep);
		addr += PAGE_SIZE;
	}
}

static int vmap_range(pgde_t *pgd, uint64_t addr, uint64_t end_addr,
		      uint64_t paddr, size_t page_size, unsigned int flags,
		      enum vmap_mode mode)
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
			pmd_phys = alloc_pmd(mode);
			if (!pmd_phys) {
				vunmap_range(pgd, start_addr, addr, mode);
				return -ENOMEM;
			}
			pmd = get_pmd_virt(pmd_phys, mode);
			pgde_set_pmd(pgdep, pmd_phys);
		} else {
			pmd = get_pmd_virt(pgde_get_pmd(*pgdep), mode);
		}
		pmdep = pmd + pmde_index(addr);
		if (page_size == PAGE_SIZE_2M) {
			assert(!pmde_present(*pmdep));
			pmde_set_large(pmdep, paddr, flags);
			continue;
		}
		if (!pmde_present(*pmdep)) {
			pt_phys = alloc_pt(mode);
			if (!pt_phys) {
				vunmap_range(pgd, start_addr, addr, mode);
				return -ENOMEM;
			}
			pt = get_pt_virt(pt_phys, mode);
			pmde_set_pt(pmdep, pt_phys);
		} else {
			pt = get_pt_virt(pmde_get_pt(*pmdep), mode);
		}
		ptep = pt + pte_index(addr);
		assert(!pte_present(*ptep));
		pte_set(ptep, paddr, flags);
	}

	return 0;
}

int vmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	 unsigned int flags, enum vmap_mode mode)
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
				 flags, mode);
		if (err)
			return err;

		addr += rem_size;
		paddr += rem_size;
	}

	return 0;
}

void vunmap(pgde_t *pgd, uint64_t addr, size_t size, enum vmap_mode mode)
{
	assert(is_aligned(addr, PAGE_SIZE));
	assert(is_aligned(size, PAGE_SIZE));
	vunmap_range(pgd, addr, addr + size, mode);
}

int kvmap(uint64_t addr, size_t size, uint64_t paddr, unsigned int flags)
{
	return kvmap_with_mode(addr, size, paddr, flags, VMAP_MODE_FINAL);
}

int kvmap_with_mode(uint64_t addr, size_t size, uint64_t paddr,
		    unsigned int flags, enum vmap_mode mode)
{
	int ret;
	size = align_up(size, PAGE_SIZE);
	spinlock_acquire(&kernel_pgdir_lock);
	ret = vmap(kernel_pgdir, addr, size, paddr, flags, mode);
	spinlock_release(&kernel_pgdir_lock);
	return ret;
}

void kvunmap(uint64_t addr, size_t size)
{
	spinlock_acquire(&kernel_pgdir_lock);
	vunmap(kernel_pgdir, addr, size, VMAP_MODE_FINAL);
	spinlock_release(&kernel_pgdir_lock);
}

int uvmap(pgde_t *pgd, uint64_t addr, size_t size, uint64_t paddr,
	  unsigned int flags)
{
	return vmap(pgd, addr, size, paddr, flags | PTE_U, VMAP_MODE_FINAL);
}

void uvunmap(pgde_t *pgd, uint64_t addr, size_t size)
{
	vunmap(pgd, addr, size, VMAP_MODE_FINAL);
}

static struct vm_area *find_vm_area(uint64_t addr)
{
	struct vm_area *area;

	list_for_each_entry(area, &vma, list)
		if (area->addr == addr)
			return area;

	return NULL;
}

static void merge_free_vm_areas(void)
{
	struct vm_area *curr, *next;

	list_for_each_entry_safe(curr, next, &vma, list) {
		if (curr->is_free && next->is_free &&
		    curr->addr + curr->size == next->addr) {
			next->addr = curr->addr;
			next->size += curr->size;
			list_del(&curr->list);
			vm_area_free(curr);
		}
	}
}

static struct vm_area *find_free_vm_area(size_t size)
{
	struct vm_area *area;
	struct vm_area *new_area;

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
			new_area = vm_area_alloc();
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

static void free_vm_area(struct vm_area *area)
{
	area->is_free = true;
	merge_free_vm_areas();
}

void vmalloc_init(void)
{
	kmem_cache_init(&vma_cache, sizeof(struct vm_area),
			alignof(struct vm_area), "vma_cache");
	list_init(&vma);
	struct vm_area *area = vm_area_alloc();
	area->addr = VMALLOC_START;
	area->size = VMALLOC_SIZE;
	area->is_free = true;
	list_add(&area->list, &vma);
}

void *vmalloc(size_t size)
{
	spinlock_acquire(&vma_lock);

	size = align_up(size, PAGE_SIZE);

	struct vm_area *area = find_free_vm_area(size);
	if (!area) {
		spinlock_release(&vma_lock);
		return NULL;
	}

	size_t npgs = size >> PAGE_SHIFT;

	area->pages = kcalloc(npgs, sizeof(struct page *));
	if (!area->pages) {
		free_vm_area(area);
		spinlock_release(&vma_lock);
		return NULL;
	}
	area->nr_pages = npgs;

	size_t i = 0;
	uint64_t vaddr = area->addr;
	for (; i < npgs; ++i) {
		struct page *pg = page_alloc(0);
		if (!pg)
			goto out_cleanup;
		area->pages[i] = pg;
		if (kvmap(vaddr, PAGE_SIZE, page_to_phys(pg), PTE_R | PTE_W)) {
			assert(pg);
			page_free(pg, 0);
			goto out_cleanup;
		}
		vaddr += PAGE_SIZE;
	}

	spinlock_release(&vma_lock);
	return (void *)area->addr;

out_cleanup:
	for (size_t j = 0; j < i; ++j) {
		assert(area->pages[j]);
		page_free(area->pages[j], 0);
	}
	kfree(area->pages);
	free_vm_area(area);
	spinlock_release(&vma_lock);
	return NULL;
}

void vfree(void *ptr)
{
	struct vm_area *area;

	spinlock_acquire(&vma_lock);
	area = find_vm_area((uint64_t)ptr);
	if (!area || area->is_free) {
		spinlock_release(&vma_lock);
		return;
	}

	kvunmap(area->addr, area->size);
	for (size_t i = 0; i < area->nr_pages; ++i) {
		assert(area->pages[i]);
		page_free(area->pages[i], 0);
	}
	kfree(area->pages);
	free_vm_area(area);
	spinlock_release(&vma_lock);
}

void *vmalloc_nomap(size_t size)
{
	struct vm_area *area;

	spinlock_acquire(&vma_lock);
	area = find_free_vm_area(align_up(size, PAGE_SIZE));
	spinlock_release(&vma_lock);
	if (!area)
		return NULL;

	return (void *)area->addr;
}

void vfree_nomap(void *ptr)
{
	struct vm_area *area;

	spinlock_acquire(&vma_lock);
	area = find_vm_area((uint64_t)ptr);
	if (!area || area->is_free) {
		spinlock_release(&vma_lock);
		return;
	}
	free_vm_area(area);
	spinlock_release(&vma_lock);
}

struct vm_area *vm_area_alloc(void)
{
	struct vm_area *vma = kmem_cache_alloc(&vma_cache);
	if (vma) {
		memset(vma, 0, sizeof(*vma));
		list_init(&vma->list);
	}
	return vma;
}

void vm_area_free(struct vm_area *area)
{
	kmem_cache_free(&vma_cache, area);
}
