#include <aosd/asm.h>
#include <aosd/errno.h>
#include <aosd/list.h>
#include <aosd/mm.h>
#include <aosd/mm_types.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/process.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/types.h>
#include <aosd/vmalloc.h>

static struct kmem_cache mm_cache;

void mm_cache_init(void)
{
	kmem_cache_init(&mm_cache, sizeof(struct mm_struct),
			alignof(struct mm_struct), "mm_cache");
}

struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm;

	mm = kmem_cache_alloc(&mm_cache);
	if (!mm)
		return NULL;

	mm->pgd = create_user_pgtable();
	if (!mm->pgd) {
		kmem_cache_free(&mm_cache, mm);
		return NULL;
	}

	list_init(&mm->seg);

	mm->stack = vm_area_alloc();
	if (!mm->stack) {
		destroy_user_pgtable(mm->pgd);
		kmem_cache_free(&mm_cache, mm);
		return NULL;
	}

	mm->heap = vm_area_alloc();
	if (!mm->heap) {
		vm_area_free(mm->stack);
		destroy_user_pgtable(mm->pgd);
		kmem_cache_free(&mm_cache, mm);
		return NULL;
	}

	mm->brk = 0;

	return mm;
}

static void mm_free_seg(struct mm_struct *mm)
{
	struct vm_area *curr, *next;

	if (list_empty(&mm->seg))
		return;

	list_for_each_entry_safe(curr, next, &mm->seg, list) {
		list_del(&curr->list);
		uvunmap(mm->pgd, curr->addr, curr->size);
		for (size_t i = 0; i < curr->nr_pages; ++i) {
			assert(curr->pages[i]);
			page_free(curr->pages[i], 0);
		}
		kfree(curr->pages);
		vm_area_free(curr);
	}
}

static void mm_free_stack(struct mm_struct *mm)
{
	if (mm->stack->size > 0) {
		uvunmap(mm->pgd, mm->stack->addr, mm->stack->size);
		assert(mm->stack->pages[0]);
		page_free(mm->stack->pages[0], USTACK_PAGE_ORDER);
	}
	vm_area_free(mm->stack);
}

static void mm_free_heap(struct mm_struct *mm)
{
	if (mm->heap->size > 0) {
		uvunmap(mm->pgd, mm->heap->addr, mm->heap->size);
		for (size_t i = 0; i < mm->heap->nr_pages; ++i) {
			assert(mm->heap->pages[i]);
			page_free(mm->heap->pages[i], 0);
		}
		kfree(mm->heap->pages);
	}
	vm_area_free(mm->heap);
}

void mm_free(struct mm_struct *mm)
{
	mm_free_seg(mm);
	mm_free_stack(mm);
	mm_free_heap(mm);
	destroy_user_pgtable(mm->pgd);
	kmem_cache_free(&mm_cache, mm);
}

static int mm_copy_area(struct vm_area *dst, struct vm_area *src,
			struct mm_struct *mm)
{
	size_t npgs;
	struct page **pgs;
	int err = 0;

	npgs = src->nr_pages;
	pgs = kcalloc(npgs, sizeof(struct page *));
	if (!pgs)
		return -ENOMEM;

	size_t i = 0;
	uint64_t addr = src->addr;
	size_t size = src->size;
	unsigned int flags = src->flags;
	while (size > 0 && i < npgs) {
		struct page *pg = page_alloc(0);
		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		uint64_t dst_pa = page_to_phys(pg);
		err = uvmap(mm->pgd, addr, PAGE_SIZE, dst_pa, flags);
		if (err) {
			assert(pg);
			page_free(pg, 0);
			goto failed;
		}
		void *dst_va = (void *)phys_to_virt(dst_pa);
		void *src_va = (void *)page_to_virt(src->pages[i]);
		memcpy(dst_va, src_va, PAGE_SIZE);
		pgs[i] = pg;
		addr += PAGE_SIZE;
		size -= PAGE_SIZE;
		++i;
	}

	dst->addr = src->addr;
	dst->size = src->size;
	dst->flags = src->flags;
	dst->pages = pgs;
	dst->nr_pages = npgs;

	return 0;

failed:
	for (uint64_t a = src->addr; a < addr; a += PAGE_SIZE)
		uvunmap(mm->pgd, a, PAGE_SIZE);
	for (size_t j = 0; j < i; ++j) {
		assert(pgs[j]);
		page_free(pgs[j], 0);
	}
	kfree(pgs);
	return err;
}

static int mm_copy_seg(struct mm_struct *dst, struct mm_struct *src)
{
	LIST_DEFINE(seg);
	struct vm_area *curr, *next;
	size_t npgs;
	struct page **pgs;
	int err = 0;

	if (list_empty(&src->seg))
		return 0;

	list_for_each_entry(curr, &src->seg, list) {
		struct vm_area *vma = vm_area_alloc();
		if (!vma) {
			err = -ENOMEM;
			goto failed;
		}
		err = mm_copy_area(vma, curr, dst);
		if (err) {
			vm_area_free(vma);
			goto failed;
		}
		list_add(&vma->list, &seg);
	}

	list_splice(&seg, &dst->seg);

	return 0;

failed:
	if (!list_empty(&seg)) {
		list_for_each_entry_safe(curr, next, &src->seg, list) {
			list_del(&curr->list);
			uvunmap(dst->pgd, curr->addr, curr->size);
			npgs = curr->nr_pages;
			pgs = curr->pages;
			for (size_t i = 0; i < npgs; ++i) {
				assert(pgs[i]);
				page_free(pgs[i], 0);
			}
			kfree(curr->pages);
			vm_area_free(curr);
		}
	}
	return err;
}

static int mm_copy_stack(struct mm_struct *dst, struct mm_struct *src)
{
	if (src->stack->size == 0)
		return 0;

	struct page *pg = page_alloc(USTACK_PAGE_ORDER);
	if (!pg)
		return -ENOMEM;

	struct page **pgs = kcalloc(1, sizeof(struct page *));
	if (!pgs) {
		assert(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return -ENOMEM;
	}

	uint64_t pa = page_to_phys(pg);
	int err = uvmap(dst->pgd, src->stack->addr, src->stack->size, pa,
			src->stack->flags);
	if (err) {
		assert(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		kfree(pgs);
		return err;
	}

	void *dst_va = (void *)phys_to_virt(pa);
	void *src_va = (void *)page_to_virt(src->stack->pages[0]);
	memcpy(dst_va, src_va, USTACK_SIZE);

	pgs[0] = pg;

	dst->stack->addr = src->stack->addr;
	dst->stack->size = src->stack->size;
	dst->stack->flags = src->stack->flags;
	dst->stack->pages = pgs;
	dst->stack->nr_pages = 1;

	return 0;
}

static int mm_copy_heap(struct mm_struct *dst, struct mm_struct *src)
{
	if (src->heap->size == 0)
		return 0;

	return mm_copy_area(dst->heap, src->heap, dst);
}

int mm_copy(struct mm_struct *dst, struct mm_struct *src)
{
	int err;

	err = mm_copy_seg(dst, src);
	if (err)
		return err;

	err = mm_copy_stack(dst, src);
	if (err) {
		mm_free_seg(dst);
		return err;
	}

	err = mm_copy_heap(dst, src);
	if (err) {
		mm_free_seg(dst);
		mm_free_stack(dst);
		return err;
	}

	return 0;
}
