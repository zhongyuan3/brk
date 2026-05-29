#include <brk/asm.h>
#include <brk/kernel.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/pgtable.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <uapi/brk/errno.h>

SPINLOCK_DEFINE(kernel_pgdir_lock);

static u64 alloc_pgtable(void)
{
	struct page *pg = page_alloc(0);
	if (!pg)
		return 0;
	u64 pa = page_to_phys(pg);
	memset((void *)phys_to_virt(pa), 0, PAGE_SIZE);
	return pa;
}

static void free_pgtable(u64 paddr)
{
	struct page *pg = phys_to_page(paddr);
	ASSERT(pg);
	page_free(pg, 0);
}

static pmde_t *pgde_to_pmd_virt(pgde_t pgde)
{
	return (pmde_t *)phys_to_virt(pgde_get_pmd(pgde));
}

static pte_t *pmde_to_pt_virt(pmde_t pmde)
{
	return (pte_t *)phys_to_virt(pmde_get_pt(pmde));
}

pgde_t *create_user_pgtable(void)
{
	u64 pa = alloc_pgtable();
	void *pgtable = (void *)phys_to_virt(pa);
	spinlock_acquire(&kernel_pgdir_lock);
	memcpy(pgtable, kernel_pgdir, PAGE_SIZE);
	spinlock_release(&kernel_pgdir_lock);
	return pgtable;
}

static int copy_user_pt(pte_t *dst, pte_t *src)
{
	u64 spg_pa;
	u64 dpg_pa;
	struct page *pg;

	for (usize_t i = 0; i < PTRS_PER_PT; ++i) {
		if (!pte_present(src[i]))
			continue;

		pg = page_alloc(0);
		if (!pg)
			return -ENOMEM;
		dpg_pa = page_to_phys(pg);
		spg_pa = pte_get(src[i]);
		void *dpg_va = (void *)phys_to_virt(dpg_pa);
		void *spg_va = (void *)phys_to_virt(spg_pa);
		memcpy(dpg_va, spg_va, PAGE_SIZE);
		pte_set(dst + i, dpg_pa, pte_flags(src[i]));
	}
	return 0;
}

static int copy_user_pmde_large(pmde_t *dst, pmde_t src)
{
	struct page *pg = page_alloc(page_order(PAGE_SIZE_2M));
	if (!pg)
		return -ENOMEM;
	u64 dpg_pa = page_to_phys(pg);
	u64 spg_pa = pmde_get_large(src);
	void *dpg_va = (void *)phys_to_virt(dpg_pa);
	void *spg_va = (void *)phys_to_virt(spg_pa);
	memcpy(dpg_va, spg_va, PAGE_SIZE_2M);
	pmde_set_large(dst, dpg_pa, pmde_flags(src));
	return 0;
}

static int copy_user_pmd(pmde_t *dst, pmde_t *src)
{
	int err = 0;

	for (usize_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (!pmde_present(src[i]))
			continue;

		if (pmde_large(src[i])) {
			err = copy_user_pmde_large(&dst[i], src[i]);
			if (err)
				return err;
			continue;
		}

		if (!pmde_present(dst[i])) {
			u64 pt_pa = alloc_pgtable();
			if (!pt_pa)
				return -ENOMEM;
			pmde_set_pt(dst + i, pt_pa);
		}

		err = copy_user_pt(pmde_to_pt_virt(dst[i]),
				   pmde_to_pt_virt(src[i]));
		if (err)
			return err;
	}

	return 0;
}

static int copy_user_pgde_large(pgde_t *dst, pgde_t src)
{
	struct page *dpg = page_alloc(page_order(PAGE_SIZE_1G));
	if (!dpg)
		return -ENOMEM;
	u64 dpg_pa = page_to_phys(dpg);
	u64 spg_pa = pgde_get_large(src);
	void *dpg_va = (void *)phys_to_virt(dpg_pa);
	void *spg_va = (void *)phys_to_virt(spg_pa);
	memcpy(dpg_va, spg_va, PAGE_SIZE_1G);
	pgde_set_large(dst, dpg_pa, pgde_flags(src));
	return 0;
}

static int copy_user_pgd(pgde_t *dst, pgde_t *src)
{
	int err = 0;

	for (usize_t i = 0; i < PTRS_PER_PGD; ++i) {
		spinlock_acquire(&kernel_pgdir_lock);
		bool is_kspace = pgde_present(kernel_pgdir[i]);
		spinlock_release(&kernel_pgdir_lock);
		if (is_kspace || !pgde_present(src[i]))
			continue;

		if (pgde_large(src[i])) {
			err = copy_user_pgde_large(&dst[i], src[i]);
			if (err)
				goto failed;
			continue;
		}

		if (!pgde_present(dst[i])) {
			u64 pmd_pa = alloc_pgtable();
			if (!pmd_pa)
				goto failed;
			pgde_set_pmd(dst + i, pmd_pa);
		}

		err = copy_user_pmd(pgde_to_pmd_virt(dst[i]),
				    pgde_to_pmd_virt(src[i]));
		if (err)
			goto failed;
	}

	return 0;

failed:
	destroy_user_pgtable(dst);
	return err;
}

int copy_user_pgtable(pgde_t *dst, pgde_t *src)
{
	return copy_user_pgd(dst, src);
}

static void destroy_user_pt(pte_t *pt, usize_t pgde_idx, usize_t pmde_idx)
{
	for (usize_t i = 0; i < PTRS_PER_PT; ++i) {
		if (pte_present(pt[i])) {
			u64 pa = pte_get(pt[i]);
			u64 va = pgde_idx << 30;
			va = va | (pmde_idx << 21);
			va = va | (i << 12);
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
			panic("%s(): pt[%lu] not empty, vaddr=%#lx,paddr=%#lx\n",
			      __func__, i, va, pa);
		}
	}
}

static void destroy_user_pmd(pmde_t *pmd, usize_t pgde_idx)
{
	u64 pa;
	pte_t *pt;

	for (usize_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (!pmde_present(pmd[i]))
			continue;

		if (pmde_large(pmd[i])) {
			pa = pmde_get_large(pmd[i]);
			u64 va = pgde_idx << 30;
			va = va | (i << 21);
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
			panic("%s(): pmd[%lu] not empty, vaddr=%#lx,paddr=%#lx\n",
			      __func__, i, va, pa);
		}

		pa = pmde_get_pt(pmd[i]);
		pt = (pte_t *)phys_to_virt(pa);
		destroy_user_pt(pt, pgde_idx, i);
		free_pgtable(pa);
	}
}

static void destroy_user_pgd(pgde_t *pgd)
{
	u64 pa;
	pmde_t *pmd;

	for (usize_t i = 0; i < PTRS_PER_PGD; ++i) {
		spinlock_acquire(&kernel_pgdir_lock);
		bool is_kspace = pgde_present(kernel_pgdir[i]);
		spinlock_release(&kernel_pgdir_lock);
		if (is_kspace || !pgde_present(pgd[i]))
			continue;

		if (pgde_large(pgd[i])) {
			pa = pgde_get_large(pgd[i]);
			u64 va = i << 30;
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
			panic("%s(): pgd[%lu] not empty, vaddr=%#lx,paddr=%#lx\n",
			      __func__, i, va, pa);
		}

		pa = pgde_get_pmd(pgd[i]);
		pmd = (pmde_t *)phys_to_virt(pa);
		destroy_user_pmd(pmd, i);
		free_pgtable(pa);
	}
}

void destroy_user_pgtable(pgde_t *pgd)
{
	destroy_user_pgd(pgd);
	free_pgtable(virt_to_phys((u64)pgd));
}
