#include <riscv/csr.h>
#include <arch/large_page.h>
#include <arch/asm/page.h>
#include <arch/pgtable.h>
#include <brk/assert.h>
#include <brk/kernel.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <uapi/errno.h>

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

static pmd_t *pgde_to_pmd_virt(pgd_t *pgdep)
{
	return (pmd_t *)phys_to_virt(pgd_next(*pgdep));
}

static pte_t *pmde_to_pt_virt(pmd_t pmde)
{
	return (pte_t *)phys_to_virt(pmd_next(pmde));
}

pgd_t *create_user_pgtable(void)
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

	for (size_t i = 0; i < PTRS_PER_PT; ++i) {
		if (!pte_is_valid(src[i]))
			continue;

		pg = page_alloc(0);
		if (!pg)
			return -ENOMEM;
		dpg_pa = page_to_phys(pg);
		spg_pa = pte_leaf(src[i]);
		void *dpg_va = (void *)phys_to_virt(dpg_pa);
		void *spg_va = (void *)phys_to_virt(spg_pa);
		memcpy(dpg_va, spg_va, PAGE_SIZE);
		pte_set_leaf(dst + i, dpg_pa, pte_flags(src[i]));
	}
	return 0;
}

static int copy_user_pmde_large(pmd_t *dst, pmd_t src)
{
	struct page *pg = page_alloc(page_order(PAGE_SIZE_2M));
	if (!pg)
		return -ENOMEM;
	u64 dpg_pa = page_to_phys(pg);
	u64 spg_pa = pmd_leaf(src);
	void *dpg_va = (void *)phys_to_virt(dpg_pa);
	void *spg_va = (void *)phys_to_virt(spg_pa);
	memcpy(dpg_va, spg_va, PAGE_SIZE_2M);
	pmd_set_leaf(dst, dpg_pa, pmd_flags(src));
	return 0;
}

static int copy_user_pmd(pmd_t *dst, pmd_t *src)
{
	int err = 0;

	for (size_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (!pmd_is_valid(src[i]))
			continue;

		if (pmd_is_leaf(src[i])) {
			err = copy_user_pmde_large(&dst[i], src[i]);
			if (err)
				return err;
			continue;
		}

		if (!pmd_is_valid(dst[i])) {
			u64 pt_pa = alloc_pgtable();
			if (!pt_pa)
				return -ENOMEM;
			pmd_set_next(dst + i, pt_pa);
		}

		err = copy_user_pt(pmde_to_pt_virt(dst[i]),
				   pmde_to_pt_virt(src[i]));
		if (err)
			return err;
	}

	return 0;
}

static int copy_user_pgde_large(pgd_t *dst, pgd_t src)
{
	struct page *dpg = page_alloc(page_order(PAGE_SIZE_1G));
	if (!dpg)
		return -ENOMEM;
	u64 dpg_pa = page_to_phys(dpg);
	u64 spg_pa = pgd_leaf(src);
	void *dpg_va = (void *)phys_to_virt(dpg_pa);
	void *spg_va = (void *)phys_to_virt(spg_pa);
	memcpy(dpg_va, spg_va, PAGE_SIZE_1G);
	pgd_set_leaf(dst, dpg_pa, pgd_flags(src));
	return 0;
}

static int copy_user_pgd(pgd_t *dst, pgd_t *src)
{
	int err = 0;

	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		spinlock_acquire(&kernel_pgdir_lock);
		bool is_kspace = pgd_is_valid(kernel_pgdir[i]);
		spinlock_release(&kernel_pgdir_lock);
		if (is_kspace || !pgd_is_valid(src[i]))
			continue;

		if (pgd_is_leaf(src[i])) {
			err = copy_user_pgde_large(&dst[i], src[i]);
			if (err)
				goto failed;
			continue;
		}

		if (!pgd_is_valid(dst[i])) {
			u64 pmd_pa = alloc_pgtable();
			if (!pmd_pa)
				goto failed;
			pgd_set_next(dst + i, pmd_pa);
		}

		err = copy_user_pmd(pgde_to_pmd_virt(&dst[i]),
				    pgde_to_pmd_virt(&src[i]));
		if (err)
			goto failed;
	}

	return 0;

failed:
	destroy_user_pgtable(dst);
	return err;
}

int copy_user_pgtable(pgd_t *dst, pgd_t *src)
{
	return copy_user_pgd(dst, src);
}

static void destroy_user_pt(pte_t *pt, size_t pgde_idx, size_t pmde_idx)
{
	for (size_t i = 0; i < PTRS_PER_PT; ++i) {
		if (pte_is_valid(pt[i])) {
			u64 pa = pte_leaf(pt[i]);
			u64 va = (pgde_idx << PGD_SHIFT) |
				 (pmde_idx << PMD_SHIFT) | (i << PAGE_SHIFT);
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
			panic("%s(): pt[%zu] not empty, vaddr=%#" PRIx64
			      ",paddr=%#" PRIx64 "\n",
			      __func__, i, va, pa);
		}
	}
}

static void destroy_user_pmd(pmd_t *pmd, size_t pgde_idx)
{
	u64 pa;
	pte_t *pt;

	for (size_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (!pmd_is_valid(pmd[i]))
			continue;

		if (pmd_is_leaf(pmd[i])) {
			pa = pmd_leaf(pmd[i]);
			u64 va = pgde_idx << 30;
			va = va | (i << 21);
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
			panic("%s(): pmd[%zu] not empty, vaddr=%#" PRIx64
			      ",paddr=%#" PRIx64 "\n",
			      __func__, i, va, pa);
		}

		pa = pmd_next(pmd[i]);
		pt = (pte_t *)phys_to_virt(pa);
		destroy_user_pt(pt, pgde_idx, i);
		free_pgtable(pa);
	}
}

static void destroy_user_pgd(pgd_t *pgd)
{
	u64 pa;
	pmd_t *pmd;

	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		spinlock_acquire(&kernel_pgdir_lock);
		bool is_kspace = pgd_is_valid(kernel_pgdir[i]);
		spinlock_release(&kernel_pgdir_lock);
		if (is_kspace || !pgd_is_valid(pgd[i]))
			continue;

		if (pgd_is_leaf(pgd[i])) {
			pa = pgd_leaf(pgd[i]);
			u64 va = i << 30;
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
			panic("%s(): pgd[%zu] not empty, vaddr=%#" PRIx64
			      ",paddr=%#" PRIx64 "\n",
			      __func__, i, va, pa);
		}

		pa = pgd_next(pgd[i]);
		pmd = (pmd_t *)phys_to_virt(pa);
		destroy_user_pmd(pmd, i);
		free_pgtable(pa);
	}
}

void destroy_user_pgtable(pgd_t *pgd)
{
	destroy_user_pgd(pgd);
	free_pgtable(virt_to_phys((uintptr_t)pgd));
}

void user_access_enable(void)
{
	write_sstatus(read_sstatus() | SSTATUS_SUM);
}

void user_access_disable(void)
{
	write_sstatus(read_sstatus() & ~SSTATUS_SUM);
}
