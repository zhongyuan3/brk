#include <arch/asm/large_page.h>
#include <arch/pgtable.h>
#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/memblock.h>
#include <brk/mm.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/vmalloc.h>
#include <uapi/errno.h>

struct vmalloc_region {
	struct list_head list;
	u64 addr;
	size_t size;
	struct page **pages;
	size_t nr_pages;
	bool is_free;
};

static struct slab_allocator vma_cache;
static struct list_head vma;
static SPINLOCK_DEFINE(vma_lock);

static struct vmalloc_region *vmalloc_region_alloc(void)
{
	struct vmalloc_region *vma = slab_alloc(&vma_cache);
	if (vma) {
		memset(vma, 0, sizeof(*vma));
		list_init(&vma->list);
	}
	return vma;
}

static void vmalloc_region_free(struct vmalloc_region *area)
{
	slab_free(&vma_cache, area);
}

static struct vmalloc_region *find_vm_area(u64 addr)
{
	struct vmalloc_region *area;

	list_for_each_entry(area, &vma, list)
		if (area->addr == addr)
			return area;

	return NULL;
}

static void merge_free_vm_areas(void)
{
	struct vmalloc_region *curr, *next;

	list_for_each_entry_safe(curr, next, &vma, list) {
		if (curr->is_free && next->is_free &&
		    curr->addr + curr->size == next->addr) {
			next->addr = curr->addr;
			next->size += curr->size;
			list_del(&curr->list);
			vmalloc_region_free(curr);
		}
	}
}

static struct vmalloc_region *find_free_vm_area(size_t size)
{
	struct vmalloc_region *area;
	struct vmalloc_region *new_area;

	size = round_up(size, PAGE_SIZE);

	list_for_each_entry(area, &vma, list) {
		if (!area->is_free)
			continue;
		if (area->size < size) {
			continue;
		} else if (area->size == size) {
			area->is_free = false;
			return area;
		} else {
			new_area = vmalloc_region_alloc();
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

static void free_vm_area(struct vmalloc_region *area)
{
	area->is_free = true;
	merge_free_vm_areas();
}

void vmalloc_init(void)
{
	slab_init(&vma_cache, sizeof(struct vmalloc_region),
		  alignof(struct vmalloc_region), "vma_cache");
	list_init(&vma);
	struct vmalloc_region *area = vmalloc_region_alloc();
	area->addr = VMALLOC_START;
	area->size = VMALLOC_SIZE;
	area->is_free = true;
	list_add(&area->list, &vma);
	pr_debug("kmalloc: %s() done\n", __func__);
}

void *vmalloc(size_t size)
{
	spin_lock(&vma_lock);

	size = round_up(size, PAGE_SIZE);

	struct vmalloc_region *area = find_free_vm_area(size);
	if (!area) {
		spin_unlock(&vma_lock);
		return NULL;
	}

	size_t npgs = size >> PAGE_SHIFT;

	area->pages = kcalloc(npgs, sizeof(struct page *));
	if (!area->pages) {
		free_vm_area(area);
		spin_unlock(&vma_lock);
		return NULL;
	}
	area->nr_pages = npgs;

	size_t i = 0;
	u64 vaddr = area->addr;
	for (; i < npgs; ++i) {
		struct page *pg = page_alloc(0);
		if (!pg)
			goto out_cleanup;
		area->pages[i] = pg;
		if (map_kernel_pages(vaddr, PAGE_SIZE, page_to_phys(pg),
				     PTE_R | PTE_W)) {
			ASSERT(pg);
			page_free(pg, 0);
			goto out_cleanup;
		}
		vaddr += PAGE_SIZE;
	}

	spin_unlock(&vma_lock);
	return (void *)(uintptr_t)area->addr;

out_cleanup:
	for (size_t j = 0; j < i; ++j) {
		ASSERT(area->pages[j]);
		page_free(area->pages[j], 0);
	}
	kfree(area->pages);
	free_vm_area(area);
	spin_unlock(&vma_lock);
	return NULL;
}

void vfree(void *ptr)
{
	struct vmalloc_region *area;

	spin_lock(&vma_lock);
	area = find_vm_area((uintptr_t)ptr);
	if (!area || area->is_free) {
		spin_unlock(&vma_lock);
		return;
	}

	unmap_kernel_pages(area->addr, area->size);
	for (size_t i = 0; i < area->nr_pages; ++i) {
		ASSERT(area->pages[i]);
		page_free(area->pages[i], 0);
	}
	kfree(area->pages);
	free_vm_area(area);
	spin_unlock(&vma_lock);
}

void *vmalloc_nomap(size_t size)
{
	struct vmalloc_region *area;

	spin_lock(&vma_lock);
	area = find_free_vm_area(round_up(size, PAGE_SIZE));
	spin_unlock(&vma_lock);
	if (!area)
		return NULL;

	return (void *)(uintptr_t)area->addr;
}

void vfree_nomap(void *ptr)
{
	struct vmalloc_region *area;

	spin_lock(&vma_lock);
	area = find_vm_area((uintptr_t)ptr);
	if (!area || area->is_free) {
		spin_unlock(&vma_lock);
		return;
	}
	free_vm_area(area);
	spin_unlock(&vma_lock);
}
