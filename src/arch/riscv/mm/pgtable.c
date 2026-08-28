#include <arch/csr.h>
#include <arch/large_page.h>
#include <arch/page.h>
#include <arch/pgtable.h>
#include <brk/base/assert.h>
#include <brk/base/kernel.h>
#include <brk/lib/string.h>
#include <brk/lock/spinlock.h>
#include <brk/mm/mm.h>
#include <brk/mm/mm_types.h>
#include <brk/mm/pgalloc.h>
#include <brk/printk/panic.h>
#include <uapi/brk/errno.h>

SPINLOCK_DEFINE(kernel_pgdir_lock);

static uint64_t alloc_pgtable(void)
{
	struct page *pg = page_alloc(0);
	if (!pg)
		return 0;
	uint64_t pa = page_to_phys(pg);
	memset((void *)phys_to_virt(pa), 0, PAGE_SIZE);
	return pa;
}

static void free_pgtable(uint64_t paddr)
{
	struct page *pg = phys_to_page(paddr);
	ASSERT(pg);
	page_free(pg, 0);
}

static pmd_t *pgde_to_pmd_virt(pgd_t *pgdep)
{
#if __riscv_xlen == 32
	/* Sv32: PMD is folded onto the PGD entry. */
	return (pmd_t *)pgdep;
#else
	return (pmd_t *)phys_to_virt(pgd_get_pmd(*pgdep));
#endif
}

static pte_t *pmde_to_pt_virt(pmd_t pmde)
{
	return (pte_t *)phys_to_virt(pmd_get_pte(pmde));
}

pgd_t *create_user_pgtable(void)
{
	uint64_t pa = alloc_pgtable();
	void *pgtable = (void *)phys_to_virt(pa);
	spinlock_acquire(&kernel_pgdir_lock);
	memcpy(pgtable, kernel_pgdir, PAGE_SIZE);
	spinlock_release(&kernel_pgdir_lock);
	return pgtable;
}

static int copy_user_pt(pte_t *dst, pte_t *src)
{
	uint64_t spg_pa;
	uint64_t dpg_pa;
	struct page *pg;

	for (size_t i = 0; i < PTRS_PER_PT; ++i) {
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

#if __riscv_xlen == 64
static int copy_user_pmde_large(pmd_t *dst, pmd_t src)
{
	struct page *pg = page_alloc(page_order(PAGE_SIZE_2M));
	if (!pg)
		return -ENOMEM;
	uint64_t dpg_pa = page_to_phys(pg);
	uint64_t spg_pa = pmd_get_large(src);
	void *dpg_va = (void *)phys_to_virt(dpg_pa);
	void *spg_va = (void *)phys_to_virt(spg_pa);
	memcpy(dpg_va, spg_va, PAGE_SIZE_2M);
	pmd_set_large(dst, dpg_pa, pmd_flags(src));
	return 0;
}
#endif

static int copy_user_pmd(pmd_t *dst, pmd_t *src)
{
	int err = 0;

	for (size_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (!pmd_present(src[i]))
			continue;

#if __riscv_xlen == 64
		if (pmd_large(src[i])) {
			err = copy_user_pmde_large(&dst[i], src[i]);
			if (err)
				return err;
			continue;
		}
#endif

		if (!pmd_present(dst[i])) {
			uint64_t pt_pa = alloc_pgtable();
			if (!pt_pa)
				return -ENOMEM;
			pmd_set_pte(dst + i, pt_pa);
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
#if __riscv_xlen == 32
	struct page *dpg = page_alloc(page_order(PAGE_SIZE_4M));
	if (!dpg)
		return -ENOMEM;
	uint64_t dpg_pa = page_to_phys(dpg);
	uint64_t spg_pa = pgd_get_large(src);
	void *dpg_va = (void *)phys_to_virt(dpg_pa);
	void *spg_va = (void *)phys_to_virt(spg_pa);
	memcpy(dpg_va, spg_va, PAGE_SIZE_4M);
	pgd_set_large(dst, dpg_pa, pgd_flags(src));
#else
	struct page *dpg = page_alloc(page_order(PAGE_SIZE_1G));
	if (!dpg)
		return -ENOMEM;
	uint64_t dpg_pa = page_to_phys(dpg);
	uint64_t spg_pa = pgd_get_large(src);
	void *dpg_va = (void *)phys_to_virt(dpg_pa);
	void *spg_va = (void *)phys_to_virt(spg_pa);
	memcpy(dpg_va, spg_va, PAGE_SIZE_1G);
	pgd_set_large(dst, dpg_pa, pgd_flags(src));
#endif
	return 0;
}

static int copy_user_pgd(pgd_t *dst, pgd_t *src)
{
	int err = 0;

	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		spinlock_acquire(&kernel_pgdir_lock);
		bool is_kspace = pgd_present(kernel_pgdir[i]);
		spinlock_release(&kernel_pgdir_lock);
		if (is_kspace || !pgd_present(src[i]))
			continue;

		if (pgd_large(src[i])) {
			err = copy_user_pgde_large(&dst[i], src[i]);
			if (err)
				goto failed;
			continue;
		}

		if (!pgd_present(dst[i])) {
			uint64_t pmd_pa = alloc_pgtable();
			if (!pmd_pa)
				goto failed;
			pgd_set_pmd(dst + i, pmd_pa);
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
		if (pte_present(pt[i])) {
			uint64_t pa = pte_get(pt[i]);
			uint64_t va = (pgde_idx << PGD_SHIFT) |
				      (pmde_idx << PMD_SHIFT) |
				      (i << PAGE_SHIFT);
#if __riscv_xlen == 64
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
#endif
			panic("%s(): pt[%zu] not empty, vaddr=%#" PRIx64
			      ",paddr=%#" PRIx64 "\n",
			      __func__, i, va, pa);
		}
	}
}

#if __riscv_xlen == 64
static void destroy_user_pmd(pmd_t *pmd, size_t pgde_idx)
{
	uint64_t pa;
	pte_t *pt;

	for (size_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (!pmd_present(pmd[i]))
			continue;

		if (pmd_large(pmd[i])) {
			pa = pmd_get_large(pmd[i]);
			uint64_t va = pgde_idx << 30;
			va = va | (i << 21);
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
			panic("%s(): pmd[%zu] not empty, vaddr=%#" PRIx64
			      ",paddr=%#" PRIx64 "\n",
			      __func__, i, va, pa);
		}

		pa = pmd_get_pte(pmd[i]);
		pt = (pte_t *)phys_to_virt(pa);
		destroy_user_pt(pt, pgde_idx, i);
		free_pgtable(pa);
	}
}
#endif

static void destroy_user_pgd(pgd_t *pgd)
{
	uint64_t pa;
#if __riscv_xlen == 64
	pmd_t *pmd;
#endif

	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		spinlock_acquire(&kernel_pgdir_lock);
		bool is_kspace = pgd_present(kernel_pgdir[i]);
		spinlock_release(&kernel_pgdir_lock);
		if (is_kspace || !pgd_present(pgd[i]))
			continue;

		if (pgd_large(pgd[i])) {
			pa = pgd_get_large(pgd[i]);
#if __riscv_xlen == 32
			uint64_t va = i << PGD_SHIFT;
#else
			uint64_t va = i << 30;
			if (va & (1UL << 38))
				va = (~((1UL << 39) - 1)) | va;
#endif
			panic("%s(): pgd[%zu] not empty, vaddr=%#" PRIx64
			      ",paddr=%#" PRIx64 "\n",
			      __func__, i, va, pa);
		}

		pa = pgd_get_pmd(pgd[i]);
#if __riscv_xlen == 32
		/* Sv32: the table below a PGD entry is a PTE table. */
		pte_t *pt = (pte_t *)phys_to_virt(pa);
		destroy_user_pt(pt, i, 0);
#else
		pmd = (pmd_t *)phys_to_virt(pa);
		destroy_user_pmd(pmd, i);
#endif
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
