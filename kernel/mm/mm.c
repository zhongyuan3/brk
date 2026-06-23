#include <arch/pgtable.h>
#include <brk/assert.h>
#include <brk/error.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <uapi/brk/errno.h>

static struct slab_allocator uvms_cache;

void uvm_space_cache_init(void)
{
	slab_init(&uvms_cache, sizeof(struct uvm_space),
		  alignof(struct uvm_space), "uvms_cache");
}

struct uvm_space *uvm_space_create(void)
{
	struct uvm_space *mm;

	mm = slab_alloc(&uvms_cache);
	if (!mm)
		return ERR_PTR(-ENOMEM);
	refcnt_init(&mm->refcnt, 1);

	mm->pgd = create_user_pgtable();
	if (!mm->pgd) {
		slab_free(&uvms_cache, mm);
		return ERR_PTR(-ENOMEM);
	}

	list_init(&mm->seg);

	mm->stack = uvm_region_alloc();
	if (!mm->stack) {
		destroy_user_pgtable(mm->pgd);
		slab_free(&uvms_cache, mm);
		return ERR_PTR(-ENOMEM);
	}

	mm->heap = uvm_region_alloc();
	if (!mm->heap) {
		uvm_region_free(mm->stack);
		destroy_user_pgtable(mm->pgd);
		slab_free(&uvms_cache, mm);
		return ERR_PTR(-ENOMEM);
	}

	mm->brk = 0;

	return mm;
}

static void uvm_space_free_segments(struct uvm_space *mm)
{
	struct uvm_region *curr, *next;

	if (list_empty(&mm->seg))
		return;

	list_for_each_entry_safe(curr, next, &mm->seg, list) {
		list_del(&curr->list);
		uvunmap(mm->pgd, curr->addr, curr->size);
		for (size_t i = 0; i < curr->nr_pages; ++i) {
			ASSERT(curr->pages[i]);
			page_free(curr->pages[i], 0);
		}
		kfree(curr->pages);
		uvm_region_free(curr);
	}
}

static void uvm_space_free_stack(struct uvm_space *mm)
{
	if (mm->stack->size > 0) {
		uvunmap(mm->pgd, mm->stack->addr, mm->stack->size);
		ASSERT(mm->stack->pages[0]);
		page_free(mm->stack->pages[0], USTACK_PAGE_ORDER);
	}
	uvm_region_free(mm->stack);
}

static void uvm_space_free_heap(struct uvm_space *mm)
{
	if (mm->heap->size > 0) {
		uvunmap(mm->pgd, mm->heap->addr, mm->heap->size);
		for (size_t i = 0; i < mm->heap->nr_pages; ++i) {
			ASSERT(mm->heap->pages[i]);
			page_free(mm->heap->pages[i], 0);
		}
		kfree(mm->heap->pages);
	}
	uvm_region_free(mm->heap);
}

static void uvm_space_destroy(struct uvm_space *mm)
{
	uvm_space_free_segments(mm);
	uvm_space_free_stack(mm);
	uvm_space_free_heap(mm);
	destroy_user_pgtable(mm->pgd);
	slab_free(&uvms_cache, mm);
}

struct uvm_space *uvm_space_get(struct uvm_space *mm)
{
	refcnt_inc(&mm->refcnt);
	return mm;
}

void uvm_space_put(struct uvm_space *mm)
{
	if (refcnt_dec_fetch(&mm->refcnt) > 0)
		return;
	uvm_space_destroy(mm);
}

static int uvm_space_copy_region(struct uvm_region *dst, struct uvm_region *src,
				 struct uvm_space *mm)
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
			ASSERT(pg);
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
		ASSERT(pgs[j]);
		page_free(pgs[j], 0);
	}
	kfree(pgs);
	return err;
}

static int uvm_space_copy_segments(struct uvm_space *dst, struct uvm_space *src)
{
	LIST_DEFINE(seg);
	struct uvm_region *curr, *next;
	size_t npgs;
	struct page **pgs;
	int err = 0;

	if (list_empty(&src->seg))
		return 0;

	list_for_each_entry(curr, &src->seg, list) {
		struct uvm_region *vma = uvm_region_alloc();
		if (!vma) {
			err = -ENOMEM;
			goto failed;
		}
		err = uvm_space_copy_region(vma, curr, dst);
		if (err) {
			uvm_region_free(vma);
			goto failed;
		}
		list_add(&vma->list, &seg);
	}

	list_splice(&seg, &dst->seg);

	return 0;

failed:
	if (!list_empty(&seg)) {
		list_for_each_entry_safe(curr, next, &seg, list) {
			list_del(&curr->list);
			uvunmap(dst->pgd, curr->addr, curr->size);
			npgs = curr->nr_pages;
			pgs = curr->pages;
			for (size_t i = 0; i < npgs; ++i) {
				ASSERT(pgs[i]);
				page_free(pgs[i], 0);
			}
			kfree(curr->pages);
			uvm_region_free(curr);
		}
	}
	return err;
}

static int uvm_space_copy_stack(struct uvm_space *dst, struct uvm_space *src)
{
	if (src->stack->size == 0)
		return 0;

	struct page *pg = page_alloc(USTACK_PAGE_ORDER);
	if (!pg)
		return -ENOMEM;

	struct page **pgs = kcalloc(1, sizeof(struct page *));
	if (!pgs) {
		ASSERT(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return -ENOMEM;
	}

	uint64_t pa = page_to_phys(pg);
	int err = uvmap(dst->pgd, src->stack->addr, src->stack->size, pa,
			src->stack->flags);
	if (err) {
		ASSERT(pg);
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

static int uvm_space_copy_heap(struct uvm_space *dst, struct uvm_space *src)
{
	if (src->heap->size == 0)
		return 0;

	return uvm_space_copy_region(dst->heap, src->heap, dst);
}

int uvm_space_copy(struct uvm_space *dst, struct uvm_space *src)
{
	int err;

	err = uvm_space_copy_segments(dst, src);
	if (err)
		return err;

	err = uvm_space_copy_stack(dst, src);
	if (err) {
		uvm_space_free_segments(dst);
		return err;
	}

	err = uvm_space_copy_heap(dst, src);
	if (err) {
		uvm_space_free_segments(dst);
		uvm_space_free_stack(dst);
		return err;
	}

	return 0;
}

struct uvm_region *uvm_region_alloc(void)
{
	struct uvm_region *reg = kzalloc(sizeof(struct uvm_region));
	if (!reg)
		return NULL;
	list_init(&reg->list);
	return reg;
}

void uvm_region_free(struct uvm_region *region)
{
	kfree(region);
}
