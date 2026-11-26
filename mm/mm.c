#include <arch/asm/huge_page.h>
#include <arch/pgtable.h>
#include <arch/pgtable_types.h>
#include <brk/align.h>
#include <brk/assert.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mchain.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/pagecache.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/refcnt.h>
#include <brk/slab.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <stdalign.h>
#include <uapi/errno.h>

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
	sleeplock_init(&mm->lock, "uvm_space");

	mm->pgd = pgd_alloc_user();
	if (!mm->pgd) {
		slab_free(&uvms_cache, mm);
		return ERR_PTR(-ENOMEM);
	}

	list_init(&mm->seg);

	mm->stack = uvm_region_alloc();
	if (!mm->stack) {
		pgd_free_user(mm->pgd);
		slab_free(&uvms_cache, mm);
		return ERR_PTR(-ENOMEM);
	}

	mm->heap = uvm_region_alloc();
	if (!mm->heap) {
		uvm_region_free(mm->stack);
		pgd_free_user(mm->pgd);
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
		assert(mm->stack->pages[0]);
		page_free(mm->stack->pages[0], USTACK_PAGE_ORDER);
	}
	uvm_region_free(mm->stack);
}

static void uvm_space_free_heap(struct uvm_space *mm)
{
	if (mm->heap->size > 0) {
		unmap_user_pages(mm->pgd, mm->heap->addr, mm->heap->size);
		for (size_t i = 0; i < mm->heap->nr_pages; ++i) {
			assert(mm->heap->pages[i]);
			page_put(mm->heap->pages[i]);
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
	pgd_free_user(mm->pgd);
	slab_free(&uvms_cache, mm);
}

void uvm_space_free(struct uvm_space *mm)
{
	if (!mm)
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
			page_put(region->pages[idx]);
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
			inode_put(region->inode);
		if (region->file)
			file_put(region->file);
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
		user_addr_t addr = region->addr + idx * PAGE_SIZE;

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
	user_addr_t addr = src->addr;
	size_t size = src->size;
	int err = 0;

	if (src->kind == UVM_REGION_SHARED_FILE) {
		dst->kind = UVM_REGION_SHARED_FILE;
		dst->inode = inode_get(src->inode);
		dst->file = file_get(src->file);
		dst->file_offset = src->file_offset;
		dst->cached = kcalloc(npgs, sizeof(dst->cached[0]));
		if (!dst->cached) {
			inode_put(dst->inode);
			dst->inode = NULL;
			if (dst->file) {
				file_put(dst->file);
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
			inode_put(dst->inode);
		dst->inode = NULL;
		if (dst->file) {
			file_put(dst->file);
			dst->file = NULL;
		}
		return err;
	}

	struct page **pgs = kcalloc(npgs, sizeof(struct page *));
	bool cow = false;

	if (!pgs)
		return -ENOMEM;

	/*
	 * Anonymous mappings are demand-paged, so untouched pages have no
	 * backing page and no PTE.  Keep them untouched in the child too.
	 */
	for (i = 0; i < npgs; i++) {
		struct page *src_pg = src->pages[i];
		unsigned int flags;
		user_addr_t page_addr = src->addr + i * PAGE_SIZE;

		if (!src_pg)
			continue;

		if (!uvm_page_is_mapped(src_mm, page_addr)) {
			/* PROT_NONE: keep a private copy with no PTE. */
			struct page *pg = page_alloc(0);

			if (!pg) {
				err = -ENOMEM;
				goto failed;
			}
			memcpy((void *)page_to_virt(pg),
			       (void *)page_to_virt(src_pg), PAGE_SIZE);
			pgs[i] = pg;
			continue;
		}

		err = uvm_pte_flags(src_mm, page_addr, &flags);
		if (err)
			goto failed;

		page_get(src_pg);
		pgs[i] = src_pg;

		if (src->flags & PTE_W) {
			/*
			 * Share the page read-only with a COW marker in both
			 * address spaces; the first store takes a private copy.
			 */
			err = map_user_pages(dst_mm->pgd, page_addr, PAGE_SIZE,
					     page_to_phys(src_pg),
					     (flags & ~PTE_W) | PTE_COW);
			if (err)
				goto failed;
			err = uvm_cow_protect(src_mm, page_addr);
			if (err)
				goto failed;
			cow = true;
		} else {
			/*
			 * Keep an existing COW marker when the source PTE was
			 * already read-only due to an earlier fork.
			 */
			if (uvm_pte_is_cow(src_mm, page_addr))
				flags |= PTE_COW;
			err = map_user_pages(dst_mm->pgd, page_addr, PAGE_SIZE,
					     page_to_phys(src_pg), flags);
			if (err)
				goto failed;
		}
	}

	dst->kind = UVM_REGION_ANON;
	dst->addr = src->addr;
	dst->size = src->size;
	dst->flags = src->flags;
	dst->pages = pgs;
	dst->nr_pages = npgs;

	if (cow)
		flush_tlb();
	return 0;

failed:
	for (size_t j = 0; j <= i && j < npgs; ++j) {
		user_addr_t page_addr = src->addr + j * PAGE_SIZE;

		if (!pgs[j])
			continue;
		if (uvm_page_is_mapped(dst_mm, page_addr))
			unmap_user_pages(dst_mm->pgd, page_addr, PAGE_SIZE);
		page_put(pgs[j]);
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
		assert(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return -ENOMEM;
	}

	phys_addr_t pa = page_to_phys(pg);
	unsigned int flags;
	int err = uvm_pte_flags(src, src->stack->addr, &flags);

	if (err) {
		page_free(pg, USTACK_PAGE_ORDER);
		kfree(pgs);
		return err;
	}
	err = map_user_pages(dst->pgd, src->stack->addr, src->stack->size, pa,
			     flags);
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

static int uvm_space_copy_heap(struct uvm_space *dst, struct uvm_space *src)
{
	if (src->heap->size == 0)
		return 0;

	return uvm_space_copy_region(dst->heap, src->heap, dst, src);
}

/*
 * The caller must hold @src->lock: copying walks and COW-protects the source
 * page tables while other threads may be faulting on them.
 */
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

static void __unmap_range(virt_addr_t addr, virt_addr_t end_addr,
			  const struct map_pages_context *ctx)
{
	pgd_t *pgde;
	p4d_t *p4de;
	pud_t *pude;
	pmd_t *pmd, *pmde;
	pte_t *pt, *pte;
	pgd_t *pgd = ctx->root;
	void *(*phys_to_virt)(phys_addr_t) = ctx->phys_to_virt;

	while (addr < end_addr) {
		pgde = pgd_offset(pgd, addr);
		if (pgd_is_leaf(*pgde)) {
			assert(end_addr - addr >= (1UL << PGDIR_SHIFT));
			pgd_clear(pgde);
			addr += 1UL << PGDIR_SHIFT;
			continue;
		}
		p4de = p4d_offset(pgde, addr, phys_to_virt);
		if (p4d_is_leaf(*p4de)) {
			assert(end_addr - addr >= (1UL << P4D_SHIFT));
			p4d_clear(p4de);
			addr += 1UL << P4D_SHIFT;
			continue;
		}
		pude = pud_offset(p4de, addr, phys_to_virt);
		if (pud_is_leaf(*pude)) {
			assert(end_addr - addr >= (1UL << PUD_SHIFT));
			pud_clear(pude);
			addr += 1UL << PUD_SHIFT;
			continue;
		}
		assert(!pud_none(*pude));
		pmd = pud_to_pmd(*pude, phys_to_virt);
		pmde = pmd_offset(pmd, addr);
		if (pmd_is_leaf(*pmde)) {
			assert(end_addr - addr >= PAGE_SIZE_2M);
			pmd_clear(pmde);
			addr += PAGE_SIZE_2M;
			continue;
		}
		assert(pmd_is_valid(*pmde));
		pt = pmd_to_pt(*pmde, phys_to_virt);
		pte = pte_offset(pt, addr);
		assert(pte_is_valid(*pte));
		pte_clear(pte);
		addr += PAGE_SIZE;
	}
}

static int map_fixed_size_pages(virt_addr_t addr, phys_addr_t paddr,
				phys_size_t size, phys_size_t page_size,
				const struct map_pages_context *ctx)
{
	pgd_t *pgde;
	p4d_t *p4de;
	pud_t *pude;
	pmd_t *pmd, *pmde;
	pte_t *pt, *pte;
	phys_addr_t table_pa;
	virt_addr_t start_addr = addr;
	pgd_t *pgd = ctx->root;
	unsigned int flags = ctx->flags;
	phys_addr_t (*alloc_frame)(void) = ctx->alloc_frame;
	void *(*phys_to_virt)(phys_addr_t) = ctx->phys_to_virt;
	virt_addr_t end_addr = addr + size;

	for (; addr < end_addr; addr += page_size, paddr += page_size) {
		pgde = pgd_offset(pgd, addr);

		if (page_size == (1UL << PGDIR_SHIFT)) {
			assert(pgd_none(*pgde));
			pgd_set_leaf(pgde, paddr, flags);
			continue;
		}
		assert(!pgd_is_leaf(*pgde));

#if HAVE_P4D
		if (pgd_none(*pgde)) {
			table_pa = alloc_frame();
			if (!table_pa) {
				__unmap_range(start_addr, addr, ctx);
				return -ENOMEM;
			}
			p4de = phys_to_virt(table_pa);
			memset(p4de, 0, PAGE_SIZE);
			pgd_set_next(pgde, table_pa);
		}
#endif
		p4de = p4d_offset(pgde, addr, phys_to_virt);

		if (page_size == (1UL << P4D_SHIFT)) {
			assert(p4d_none(*p4de));
			p4d_set_leaf(p4de, paddr, flags);
			continue;
		}
		assert(!p4d_is_leaf(*p4de));

#if HAVE_PUD
		if (p4d_none(*p4de)) {
			table_pa = alloc_frame();
			if (!table_pa) {
				__unmap_range(start_addr, addr, ctx);
				return -ENOMEM;
			}
			pude = phys_to_virt(table_pa);
			memset(pude, 0, PAGE_SIZE);
			p4d_set_next(p4de, table_pa);
		}
#endif
		pude = pud_offset(p4de, addr, phys_to_virt);

		if (page_size == (1UL << PUD_SHIFT)) {
			assert(pud_none(*pude));
			pud_set_leaf(pude, paddr, flags);
			continue;
		}
		assert(!pud_is_leaf(*pude));

		if (pud_none(*pude)) {
			table_pa = alloc_frame();
			if (!table_pa) {
				__unmap_range(start_addr, addr, ctx);
				return -ENOMEM;
			}
			pmd = phys_to_virt(table_pa);
			memset(pmd, 0, PAGE_SIZE);
			pud_set_next(pude, table_pa);
		}
		pmd = pud_to_pmd(*pude, phys_to_virt);
		pmde = pmd_offset(pmd, addr);
		if (page_size == PAGE_SIZE_2M) {
			assert(!pmd_is_valid(*pmde));
			pmd_set_leaf(pmde, paddr, flags);
			continue;
		}
		if (pmd_none(*pmde)) {
			table_pa = alloc_frame();
			if (!table_pa) {
				__unmap_range(start_addr, addr, ctx);
				return -ENOMEM;
			}
			pt = phys_to_virt(table_pa);
			memset(pt, 0, PAGE_SIZE);
			pmd_set_next(pmde, table_pa);
		}
		pt = pmd_to_pt(*pmde, phys_to_virt);
		pte = pte_offset(pt, addr);
		assert(!pte_is_valid(*pte));
		pte_set_leaf(pte, paddr, flags);
	}

	return 0;
}

int map_pages(const struct map_pages_context *ctx)
{
	phys_size_t rem_size;
	phys_size_t page_size;
	int err;
	virt_addr_t addr = ctx->addr;
	virt_addr_t end_addr;
	phys_size_t size = ctx->size;
	phys_addr_t paddr = ctx->paddr;

	assert(ctx->root);
	assert(ctx->alloc_frame);
	assert(ctx->phys_to_virt);
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

		err = map_fixed_size_pages(addr, paddr, rem_size, page_size,
					   ctx);
		if (err)
			return err;

		addr += rem_size;
		paddr += rem_size;
	}

	return 0;
}

static phys_addr_t alloc_pgtable_frame(void)
{
	struct page *pg = page_alloc(0);

	if (!pg)
		return 0;

	return page_to_phys(pg);
}

static int map_pages_final(pgd_t *root, virt_addr_t addr, phys_size_t size,
			   phys_addr_t paddr, unsigned int flags)
{
	struct map_pages_context ctx = {
		.root = root,
		.addr = addr,
		.paddr = paddr,
		.size = size,
		.flags = flags,
		.alloc_frame = alloc_pgtable_frame,
		.phys_to_virt = pgtable_phys_to_virt,
	};

	return map_pages(&ctx);
}

void unmap_pages(pgd_t *root, virt_addr_t addr, phys_size_t size)
{
	struct map_pages_context ctx = {
		.root = root,
		.phys_to_virt = pgtable_phys_to_virt,
	};
	virt_addr_t start = addr;

	assert(is_aligned(start, PAGE_SIZE));
	assert(is_aligned(size, PAGE_SIZE));
	__unmap_range(start, start + size, &ctx);
}

int map_kernel_pages(virt_addr_t addr, phys_size_t size, phys_addr_t paddr,
		     unsigned int flags)
{
	int err;

	size = align_up(size, PAGE_SIZE);
	spin_lock(&kernel_pgdir_lock);
	err = map_pages_final(kernel_pgdir, addr, size, paddr, flags);
	spin_unlock(&kernel_pgdir_lock);
	return err;
}

void unmap_kernel_pages(virt_addr_t addr, phys_size_t size)
{
	spin_lock(&kernel_pgdir_lock);
	unmap_pages(kernel_pgdir, addr, size);
	spin_unlock(&kernel_pgdir_lock);
}

int map_user_pages(pgd_t *root, virt_addr_t addr, phys_size_t size,
		   phys_addr_t paddr, unsigned int flags)
{
	size = align_up(size, PAGE_SIZE);
	return map_pages_final(root, addr, size, paddr, flags | PTE_U);
}

void unmap_user_pages(pgd_t *root, virt_addr_t addr, phys_size_t size)
{
	unmap_pages(root, addr, size);
}

static void mchain_release_to_buddy(void)
{
	struct mchain_iter it;
	phys_addr_t start;
	phys_addr_t end;

#ifndef NDEBUG
	assert(mchain_validate() == 0);
#endif

	/*
	 * Freeze first so the iterator cannot race with an mchain allocation,
	 * then hand every remaining FREE range to the buddy allocator.
	 */
	mchain_freeze();
	mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
	while (mchain_iter_next(&it, &start, &end))
		page_free_phys_range(start, end - start);
}

void mm_init(void)
{
	page_alloc_init();
	mchain_release_to_buddy();
	kmalloc_init();
	vmalloc_init();
}
