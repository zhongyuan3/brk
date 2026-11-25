#include <arch/asm/page.h>
#include <arch/pgtable.h>
#include <brk/assert.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/panic.h>
#include <brk/pgalloc.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <inttypes.h>
#include <riscv/csr.h>
#include <uapi/errno.h>

SPINLOCK_DEFINE(kernel_pgdir_lock);

static phys_addr_t alloc_pgtable(void)
{
	struct page *pg = page_alloc(0);
	if (!pg)
		return 0;
	phys_addr_t pa = page_to_phys(pg);
	memset((void *)phys_to_virt(pa), 0, PAGE_SIZE);
	return pa;
}

static void free_pgtable(phys_addr_t paddr)
{
	struct page *pg = phys_to_page(paddr);
	assert(pg);
	page_free(pg, 0);
}

static pte_t *pmde_to_pt_virt(pmd_t pmde)
{
	return pmd_to_pt(pmde, pgtable_phys_to_virt);
}

pgd_t *pgd_alloc_user(void)
{
	phys_addr_t pa = alloc_pgtable();
	void *pgtable = (void *)phys_to_virt(pa);
	spin_lock(&kernel_pgdir_lock);
	memcpy(pgtable, kernel_pgdir, PAGE_SIZE);
	spin_unlock(&kernel_pgdir_lock);
	return pgtable;
}

static int copy_user_large(u64 *dst, u64 src, unsigned int shift,
			   unsigned int flags)
{
	unsigned long size = 1UL << shift;
	unsigned int order = page_order(size);
	struct page *pg;
	phys_addr_t dst_pa;
	phys_addr_t src_pa = PGTABLE_LEAF(src);

	if (((unsigned long)PAGE_SIZE << order) != size)
		return -ENOMEM;

	pg = page_alloc(order);
	if (!pg)
		return -ENOMEM;
	dst_pa = page_to_phys(pg);
	memcpy((void *)phys_to_virt(dst_pa), (void *)phys_to_virt(src_pa),
	       size);
	*dst = PGTABLE_MK(dst_pa) | PTE_V | flags;
	return 0;
}

static int copy_user_pt(pte_t *dst, pte_t *src)
{
	phys_addr_t spg_pa;
	phys_addr_t dpg_pa;
	struct page *pg;

	for (size_t i = 0; i < PTRS_PER_PT; ++i) {
		if (pte_none(src[i]))
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

static int copy_user_pmd(pmd_t *dst, pmd_t *src)
{
	int err;

	for (size_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (pmd_none(src[i]))
			continue;

		if (pmd_is_leaf(src[i])) {
			err = copy_user_large((u64 *)&dst[i], pmd_val(src[i]),
					      PMD_SHIFT, pmd_flags(src[i]));
			if (err)
				return err;
			continue;
		}

		if (pmd_none(dst[i])) {
			phys_addr_t pt_pa = alloc_pgtable();
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

#if HAVE_PUD
static int copy_user_pud(pud_t *dst, pud_t *src)
{
	int err;

	for (size_t i = 0; i < PTRS_PER_PUD; ++i) {
		if (pud_none(src[i]))
			continue;

		if (pud_is_leaf(src[i])) {
			err = copy_user_large((u64 *)&dst[i], pud_val(src[i]),
					      PUD_SHIFT, pud_flags(src[i]));
			if (err)
				return err;
			continue;
		}

		if (pud_none(dst[i])) {
			phys_addr_t pmd_pa = alloc_pgtable();
			if (!pmd_pa)
				return -ENOMEM;
			pud_set_next(dst + i, pmd_pa);
		}

		err = copy_user_pmd(pud_to_pmd(dst[i], pgtable_phys_to_virt),
				    pud_to_pmd(src[i], pgtable_phys_to_virt));
		if (err)
			return err;
	}

	return 0;
}
#endif

#if HAVE_P4D
static int copy_user_p4d(p4d_t *dst, p4d_t *src)
{
	int err;

	for (size_t i = 0; i < PTRS_PER_P4D; ++i) {
		if (p4d_none(src[i]))
			continue;

		if (p4d_is_leaf(src[i])) {
			err = copy_user_large((u64 *)&dst[i], p4d_val(src[i]),
					      P4D_SHIFT, p4d_flags(src[i]));
			if (err)
				return err;
			continue;
		}

		if (p4d_none(dst[i])) {
			phys_addr_t pud_pa = alloc_pgtable();
			if (!pud_pa)
				return -ENOMEM;
			p4d_set_next(dst + i, pud_pa);
		}

		err = copy_user_pud(p4d_to_pud(dst[i], pgtable_phys_to_virt),
				    p4d_to_pud(src[i], pgtable_phys_to_virt));
		if (err)
			return err;
	}

	return 0;
}
#endif

static int copy_user_pgd(pgd_t *dst, pgd_t *src)
{
	int err = 0;

	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		spin_lock(&kernel_pgdir_lock);
		bool is_kspace = pgd_is_valid(kernel_pgdir[i]);
		spin_unlock(&kernel_pgdir_lock);
		if (is_kspace || pgd_none(src[i]))
			continue;

		if (pgd_is_leaf(src[i])) {
			err = copy_user_large((u64 *)&dst[i], pgd_val(src[i]),
					      PGDIR_SHIFT, pgd_flags(src[i]));
			if (err)
				goto failed;
			continue;
		}

		if (pgd_none(dst[i])) {
			phys_addr_t next_pa = alloc_pgtable();
			if (!next_pa)
				goto failed;
			pgd_set_next(dst + i, next_pa);
		}

#if HAVE_P4D
		err = copy_user_p4d((p4d_t *)phys_to_virt(pgd_next(src[i])),
				    (p4d_t *)phys_to_virt(pgd_next(dst[i])));
#elif HAVE_PUD
		err = copy_user_pud((pud_t *)phys_to_virt(pgd_next(src[i])),
				    (pud_t *)phys_to_virt(pgd_next(dst[i])));
#else
		err = copy_user_pmd((pmd_t *)phys_to_virt(pgd_next(src[i])),
				    (pmd_t *)phys_to_virt(pgd_next(dst[i])));
#endif
		if (err)
			goto failed;
	}

	return 0;

failed:
	pgd_free_user(dst);
	return err;
}

int pgd_copy_user(pgd_t *dst, pgd_t *src)
{
	return copy_user_pgd(dst, src);
}

static void destroy_user_pt(pte_t *pt)
{
	for (size_t i = 0; i < PTRS_PER_PT; ++i) {
		if (pte_is_valid(pt[i]))
			panic("%s(): pt[%zu] not empty, paddr=%#" PRIx64 "\n",
			      __func__, i, (u64)pte_leaf(pt[i]));
	}
}

static void destroy_user_pmd(pmd_t *pmd)
{
	phys_addr_t pa;
	pte_t *pt;

	for (size_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (pmd_none(pmd[i]))
			continue;

		if (pmd_is_leaf(pmd[i]))
			panic("%s(): pmd[%zu] not empty, paddr=%#" PRIx64 "\n",
			      __func__, i, (u64)pmd_leaf(pmd[i]));

		pa = pmd_next(pmd[i]);
		pt = (pte_t *)phys_to_virt(pa);
		destroy_user_pt(pt);
		free_pgtable(pa);
	}
}

#if HAVE_PUD
static void destroy_user_pud(pud_t *pud)
{
	phys_addr_t pa;
	pmd_t *pmd;

	for (size_t i = 0; i < PTRS_PER_PUD; ++i) {
		if (pud_none(pud[i]))
			continue;

		if (pud_is_leaf(pud[i]))
			panic("%s(): pud[%zu] not empty, paddr=%#" PRIx64 "\n",
			      __func__, i, (u64)pud_leaf(pud[i]));

		pa = pud_next(pud[i]);
		pmd = (pmd_t *)phys_to_virt(pa);
		destroy_user_pmd(pmd);
		free_pgtable(pa);
	}
}
#endif

#if HAVE_P4D
static void destroy_user_p4d(p4d_t *p4d)
{
	phys_addr_t pa;
	pud_t *pud;

	for (size_t i = 0; i < PTRS_PER_P4D; ++i) {
		if (p4d_none(p4d[i]))
			continue;

		if (p4d_is_leaf(p4d[i]))
			panic("%s(): p4d[%zu] not empty, paddr=%#" PRIx64 "\n",
			      __func__, i, (u64)p4d_leaf(p4d[i]));

		pa = p4d_next(p4d[i]);
		pud = (pud_t *)phys_to_virt(pa);
		destroy_user_pud(pud);
		free_pgtable(pa);
	}
}
#endif

static void destroy_user_pgd(pgd_t *pgd)
{
	phys_addr_t pa;

	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		spin_lock(&kernel_pgdir_lock);
		bool is_kspace = pgd_is_valid(kernel_pgdir[i]);
		spin_unlock(&kernel_pgdir_lock);
		if (is_kspace || pgd_none(pgd[i]))
			continue;

		if (pgd_is_leaf(pgd[i]))
			panic("%s(): pgd[%zu] not empty, paddr=%#" PRIx64 "\n",
			      __func__, i, (u64)pgd_leaf(pgd[i]));

		pa = pgd_next(pgd[i]);
#if HAVE_P4D
		destroy_user_p4d((p4d_t *)phys_to_virt(pa));
#elif HAVE_PUD
		destroy_user_pud((pud_t *)phys_to_virt(pa));
#else
		destroy_user_pmd((pmd_t *)phys_to_virt(pa));
#endif
		free_pgtable(pa);
	}
}

void pgd_free_user(pgd_t *pgd)
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
