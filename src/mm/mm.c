#include <arch/asm/large_page.h>
#include <arch/pgtable.h>
#include <arch/pgtable_types.h>
#include <brk/assert.h>
#include <brk/error.h>
#include <brk/initcall.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/memblock.h>
#include <brk/fs.h>
#include <brk/mm.h>
#include <brk/pagecache.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <uapi/errno.h>

static struct slab_allocator uvms_cache;

void uvm_space_cache_init(void)
{
	slab_init(&uvms_cache, sizeof(struct uvm_space),
		  alignof(struct uvm_space), "uvms_cache");
}
core_initcall(uvm_space_cache_init);

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
	mm->mmap_top = 0;

	return mm;
}

static void uvm_space_free_segments(struct uvm_space *mm)
{
	struct uvm_region *curr, *next;

	if (list_empty(&mm->seg))
		return;

	list_for_each_entry_safe(curr, next, &mm->seg, list) {
		list_del(&curr->list);
		uvm_region_unmap_range(mm, curr, 0, curr->nr_pages);
		uvm_region_release(curr);
	}
}

static void uvm_space_free_stack(struct uvm_space *mm)
{
	if (mm->stack->size > 0) {
		unmap_user_pages(mm->pgd, mm->stack->addr, mm->stack->size);
		ASSERT(mm->stack->pages[0]);
		page_free(mm->stack->pages[0], USTACK_PAGE_ORDER);
	}
	uvm_region_free(mm->stack);
}

static void uvm_space_free_heap(struct uvm_space *mm)
{
	if (mm->heap->size > 0) {
		unmap_user_pages(mm->pgd, mm->heap->addr, mm->heap->size);
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

void uvm_region_free_range(struct uvm_region *region, size_t start, size_t nr)
{
	if (!region || start >= region->nr_pages)
		return;
	if (nr > region->nr_pages - start)
		nr = region->nr_pages - start;

	for (size_t i = 0; i < nr; i++) {
		size_t idx = start + i;

		if (region->kind == UVM_REGION_SHARED_FILE) {
			struct cached_page *cp = region->cached[idx];

			if (cp) {
				/*
				 * Hardware dirty bits are not tracked, so a
				 * shared page may have been written before a
				 * later mprotect made this VMA read-only.
				 * Write it back unconditionally.
				 */
				cached_page_mark_dirty(cp);
				cached_page_put(cp);
				region->cached[idx] = NULL;
			}
		} else if (region->pages[idx]) {
			page_free(region->pages[idx], 0);
			region->pages[idx] = NULL;
		}
	}
}

void uvm_region_release(struct uvm_region *region)
{
	if (!region)
		return;

	uvm_region_free_range(region, 0, region->nr_pages);
	kfree(region->pages);
	kfree(region->cached);

	if (region->kind == UVM_REGION_SHARED_FILE) {
		if (region->inode)
			fs_inode_put(region->inode);
		if (region->file)
			fs_file_put(region->file);
	}

	uvm_region_free(region);
}

void uvm_region_unmap_range(struct uvm_space *mm, struct uvm_region *region,
			    size_t start, size_t nr)
{
	if (!mm || !region || start >= region->nr_pages)
		return;
	if (nr > region->nr_pages - start)
		nr = region->nr_pages - start;

	for (size_t i = 0; i < nr; i++) {
		size_t idx = start + i;
		u64 addr = region->addr + idx * PAGE_SIZE;

		if (uvm_page_is_mapped(mm, addr))
			unmap_user_pages(mm->pgd, addr, PAGE_SIZE);
	}
}

static int uvm_space_copy_region(struct uvm_region *dst, struct uvm_region *src,
				 struct uvm_space *dst_mm,
				 struct uvm_space *src_mm)
{
	size_t npgs = src->nr_pages;
	size_t i = 0;
	u64 addr = src->addr;
	size_t size = src->size;
	int err = 0;

	if (src->kind == UVM_REGION_SHARED_FILE) {
		dst->kind = UVM_REGION_SHARED_FILE;
		dst->inode = fs_inode_get(src->inode);
		dst->file = fs_file_get(src->file);
		dst->file_offset = src->file_offset;
		dst->cached = kcalloc(npgs, sizeof(dst->cached[0]));
		if (!dst->cached) {
			fs_inode_put(dst->inode);
			dst->inode = NULL;
			if (dst->file) {
				fs_file_put(dst->file);
				dst->file = NULL;
			}
			return -ENOMEM;
		}

		while (size > 0 && i < npgs) {
			struct cached_page *cp = src->cached[i];
			unsigned int flags = 0;
			bool mapped;

			if (!cp) {
				err = -EIO;
				goto failed_shared;
			}

			cached_page_get(cp);
			mapped = uvm_page_is_mapped(src_mm, addr);
			if (mapped) {
				err = uvm_pte_flags(src_mm, addr, &flags);
				if (err) {
					cached_page_put(cp);
					goto failed_shared;
				}
				err = map_user_pages(dst_mm->pgd, addr,
						     PAGE_SIZE,
						     page_to_phys(cp->page),
						     flags);
				if (err) {
					cached_page_put(cp);
					goto failed_shared;
				}
			}
			dst->cached[i] = cp;
			dst->nr_pages = i + 1;
			addr += PAGE_SIZE;
			size -= PAGE_SIZE;
			++i;
		}

		dst->addr = src->addr;
		dst->size = src->size;
		dst->flags = src->flags;
		return 0;

	failed_shared:
		uvm_region_unmap_range(dst_mm, dst, 0, dst->nr_pages);
		uvm_region_free_range(dst, 0, dst->nr_pages);
		kfree(dst->cached);
		dst->cached = NULL;
		if (dst->inode)
			fs_inode_put(dst->inode);
		dst->inode = NULL;
		if (dst->file) {
			fs_file_put(dst->file);
			dst->file = NULL;
		}
		return err;
	}

	struct page **pgs = kcalloc(npgs, sizeof(struct page *));

	if (!pgs)
		return -ENOMEM;

	/*
	 * Anonymous mappings are demand-paged, so untouched pages have no
	 * backing page and no PTE.  Keep them untouched in the child too.
	 */
	for (i = 0; i < npgs; i++) {
		struct page *pg;
		unsigned int flags;
		u64 page_addr = src->addr + i * PAGE_SIZE;
		u64 dst_pa;

		if (!src->pages[i])
			continue;

		pg = page_alloc(0);
		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		dst_pa = page_to_phys(pg);
		memcpy((void *)phys_to_virt(dst_pa),
		       (void *)page_to_virt(src->pages[i]), PAGE_SIZE);
		pgs[i] = pg;

		if (!uvm_page_is_mapped(src_mm, page_addr))
			continue;
		err = uvm_pte_flags(src_mm, page_addr, &flags);
		if (err)
			goto failed;
		err = map_user_pages(dst_mm->pgd, page_addr, PAGE_SIZE,
				     dst_pa, flags);
		if (err)
			goto failed;
	}

	dst->kind = UVM_REGION_ANON;
	dst->addr = src->addr;
	dst->size = src->size;
	dst->flags = src->flags;
	dst->pages = pgs;
	dst->nr_pages = npgs;
	return 0;

failed:
	for (size_t j = 0; j < i; ++j) {
		if (!pgs[j])
			continue;
		if (uvm_page_is_mapped(dst_mm,
				       src->addr + j * PAGE_SIZE))
			unmap_user_pages(dst_mm->pgd,
					 src->addr + j * PAGE_SIZE,
					 PAGE_SIZE);
		page_free(pgs[j], 0);
	}
	kfree(pgs);
	return err;
}

static int uvm_space_copy_segments(struct uvm_space *dst, struct uvm_space *src)
{
	LIST_DEFINE(seg);
	struct uvm_region *curr, *next;
	int err = 0;

	if (list_empty(&src->seg))
		return 0;

	list_for_each_entry(curr, &src->seg, list) {
		struct uvm_region *vma = uvm_region_alloc();
		if (!vma) {
			err = -ENOMEM;
			goto failed;
		}
		err = uvm_space_copy_region(vma, curr, dst, src);
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
			uvm_region_unmap_range(dst, curr, 0, curr->nr_pages);
			uvm_region_release(curr);
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

	u64 pa = page_to_phys(pg);
	unsigned int flags;
	int err = uvm_pte_flags(src, src->stack->addr, &flags);

	if (err) {
		page_free(pg, USTACK_PAGE_ORDER);
		kfree(pgs);
		return err;
	}
	err = map_user_pages(dst->pgd, src->stack->addr, src->stack->size,
			     pa, flags);
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

	return uvm_space_copy_region(dst->heap, src->heap, dst, src);
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

	dst->brk = src->brk;
	dst->mmap_top = src->mmap_top;
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

static void __unmap_range(unsigned long addr, unsigned long end_addr,
			  const struct map_pages_context *ctx)
{
	pgd_t *pgde;
	pmd_t *pmd, *pmde;
	pte_t *pt, *pte;
	pgd_t *pgd = ctx->root;
	void *(*phys_to_virt)(unsigned long) = ctx->phys_to_virt;

	while (addr < end_addr) {
		pgde = pgd + pgd_index(addr);
		if (pgd_is_leaf(*pgde)) {
			ASSERT(end_addr - addr >= PAGE_SIZE_1G);
			pgd_clear(pgde);
			addr += PAGE_SIZE_1G;
			continue;
		}
		ASSERT(pgd_is_valid(*pgde));
		pmd = phys_to_virt(pgd_next(*pgde));
		pmde = pmd + pmd_index(addr);
		if (pmd_is_leaf(*pmde)) {
			ASSERT(end_addr - addr >= PAGE_SIZE_2M);
			pmd_clear(pmde);
			addr += PAGE_SIZE_2M;
			continue;
		}
		ASSERT(pmd_is_valid(*pmde));
		pt = phys_to_virt(pmd_next(*pmde));
		pte = pt + pte_index(addr);
		ASSERT(pte_is_valid(*pte));
		pte_clear(pte);
		addr += PAGE_SIZE;
	}
}

static int map_fixed_size_pages(unsigned long addr, unsigned long paddr,
				unsigned long size, size_t page_size,
				const struct map_pages_context *ctx)
{
	pgd_t *pgde;
	pmd_t *pmd, *pmde;
	pte_t *pt, *pte;
	u64 pmd_pa, pt_pa;
	u64 start_addr = addr;
	pgd_t *pgd = ctx->root;
	unsigned int flags = ctx->flags;
	unsigned long (*alloc_frame)(void) = ctx->alloc_frame;
	void *(*phys_to_virt)(unsigned long) = ctx->phys_to_virt;
	unsigned long end_addr = addr + size;

	for (; addr < end_addr; addr += page_size, paddr += page_size) {
		pgde = pgd + pgd_index(addr);
		if (page_size == PAGE_SIZE_1G) {
			ASSERT(!pgd_is_valid(*pgde));
			pgd_set_leaf(pgde, paddr, flags);
			continue;
		}
		if (!pgd_is_valid(*pgde)) {
			pmd_pa = alloc_frame();
			if (!pmd_pa) {
				__unmap_range(start_addr, addr, ctx);
				return -ENOMEM;
			}
			pmd = phys_to_virt(pmd_pa);
			memset(pmd, 0, PAGE_SIZE);
			pgd_set_next(pgde, pmd_pa);
		} else {
			pmd = phys_to_virt(pgd_next(*pgde));
		}
		pmde = pmd + pmd_index(addr);
		if (page_size == PAGE_SIZE_2M) {
			ASSERT(!pmd_is_valid(*pmde));
			pmd_set_leaf(pmde, paddr, flags);
			continue;
		}
		if (!pmd_is_valid(*pmde)) {
			pt_pa = alloc_frame();
			if (!pt_pa) {
				__unmap_range(start_addr, addr, ctx);
				return -ENOMEM;
			}
			pt = phys_to_virt(pt_pa);
			memset(pt, 0, PAGE_SIZE);
			pmd_set_next(pmde, pt_pa);
		} else {
			pt = phys_to_virt(pmd_next(*pmde));
		}
		pte = pt + pte_index(addr);
		ASSERT(!pte_is_valid(*pte));
		pte_set_leaf(pte, paddr, flags);
	}

	return 0;
}

int map_pages(const struct map_pages_context *ctx)
{
	size_t rem_size;
	size_t page_size;
	int err;
	unsigned long addr = (unsigned long)ctx->addr;
	unsigned long end_addr;
	unsigned long size = ctx->size;
	unsigned long paddr = ctx->paddr;

	ASSERT(ctx->root);
	ASSERT(ctx->alloc_frame);
	ASSERT(ctx->phys_to_virt);
	ASSERT(is_aligned(addr, PAGE_SIZE));
	ASSERT(is_aligned(size, PAGE_SIZE));
	ASSERT(is_aligned(paddr, PAGE_SIZE));

	end_addr = addr + size;
	while (addr < end_addr) {
		rem_size = end_addr - addr;

		if (is_aligned(addr, PAGE_SIZE_1G) &&
		    is_aligned(paddr, PAGE_SIZE_1G) &&
		    rem_size >= PAGE_SIZE_1G) {
			page_size = PAGE_SIZE_1G;
			rem_size = round_down(rem_size, PAGE_SIZE_1G);
		} else if (is_aligned(addr, PAGE_SIZE_2M) &&
			   is_aligned(paddr, PAGE_SIZE_2M) &&
			   rem_size >= PAGE_SIZE_2M) {
			page_size = PAGE_SIZE_2M;
			rem_size = round_down(rem_size, PAGE_SIZE_2M);
		} else {
			page_size = PAGE_SIZE;
			rem_size = round_down(rem_size, PAGE_SIZE);
		}

		err = map_fixed_size_pages(addr, paddr, rem_size, page_size,
					   ctx);
		if (err)
			return err;

		addr += rem_size;
		paddr += rem_size;
	}

	return 0;
}

static unsigned long alloc_pgtable_frame(void)
{
	struct page *pg = page_alloc(0);

	if (!pg)
		return 0;

	return page_to_phys(pg);
}

static void *pgtable_phys_to_virt(unsigned long paddr)
{
	return (void *)phys_to_virt(paddr);
}

static int map_pages_final(pgd_t *root, unsigned long addr,
			   unsigned long size, unsigned long paddr,
			   unsigned int flags)
{
	struct map_pages_context ctx = {
		.root = root,
		.addr = (void *)addr,
		.paddr = paddr,
		.size = size,
		.flags = flags,
		.alloc_frame = alloc_pgtable_frame,
		.phys_to_virt = pgtable_phys_to_virt,
	};

	return map_pages(&ctx);
}

void unmap_pages(pgd_t *root, void *addr, unsigned long size)
{
	struct map_pages_context ctx = {
		.root = root,
		.phys_to_virt = pgtable_phys_to_virt,
	};
	unsigned long start = (unsigned long)addr;

	ASSERT(is_aligned(start, PAGE_SIZE));
	ASSERT(is_aligned(size, PAGE_SIZE));
	__unmap_range(start, start + size, &ctx);
}

int map_kernel_pages(unsigned long addr, size_t size, unsigned long paddr,
		     unsigned int flags)
{
	int err;

	size = round_up(size, PAGE_SIZE);
	spin_lock(&kernel_pgdir_lock);
	err = map_pages_final(kernel_pgdir, addr, size, paddr, flags);
	spin_unlock(&kernel_pgdir_lock);
	return err;
}

void unmap_kernel_pages(unsigned long addr, size_t size)
{
	spin_lock(&kernel_pgdir_lock);
	unmap_pages(kernel_pgdir, (void *)addr, size);
	spin_unlock(&kernel_pgdir_lock);
}

int map_user_pages(pgd_t *root, unsigned long addr, size_t size,
		   unsigned long paddr, unsigned int flags)
{
	size = round_up(size, PAGE_SIZE);
	return map_pages_final(root, addr, size, paddr, flags | PTE_U);
}

void unmap_user_pages(pgd_t *root, unsigned long addr, size_t size)
{
	unmap_pages(root, (void *)addr, size);
}

int mm_core_init(void)
{
	page_alloc_init();
	memblock_free_all();
	kmalloc_init();
	vmalloc_init();

	memblock_dump_all();

	return 0;
}
