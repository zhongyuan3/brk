#include <aosd/align.h>
#include <aosd/mm.h>
#include <aosd/panic.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/string.h>

static uint64_t alloc_pgtable_before_buddy(void)
{
	struct page *page = page_alloc(0);
	if (!page)
		return 0;
	uint64_t paddr = page_to_phys(page);
	memset((void *)phys_to_virt(paddr), 0, PAGE_SIZE);
	return paddr;
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
	struct page *page = page_alloc(0);
	if (!page)
		return NULL;
	void *pgtable = (void *)page_to_virt(page);
	memcpy(pgtable, kernel_pgdir, PAGE_SIZE);
	return pgtable;
}

static int copy_user_pt(pte_t *dst, pte_t *src)
{
	uint64_t sphys;
	uint64_t dphys;
	struct page *page;

	for (size_t i = 0; i < PTRS_PER_PT; ++i) {
		if (!pte_present(src[i]))
			continue;

		page = page_alloc(0);
		if (!page)
			return -1;
		dphys = page_to_phys(page);
		sphys = pte_get(src[i]);
		memcpy((void *)phys_to_virt(dphys), (void *)phys_to_virt(sphys),
		       PAGE_SIZE);
		pte_set(dst + i, dphys, pte_flags(src[i]));
	}
	return 0;
}

static int copy_user_pmde_large(pmde_t *dst, pmde_t src)
{
	struct page *page = page_alloc(page_order(PAGE_SIZE_2M));
	if (!page)
		return -1;
	uint64_t dphys = page_to_phys(page);
	uint64_t sphys = pmde_get_large(src);
	memcpy((void *)phys_to_virt(dphys), (void *)phys_to_virt(sphys),
	       PAGE_SIZE_2M);
	pmde_set_large(dst, dphys, pmde_flags(src));
	return 0;
}

static int copy_user_pmd(pmde_t *dst, pmde_t *src)
{
	for (size_t i = 0; i < PTRS_PER_PMD; ++i) {
		if (!pmde_present(src[i]))
			continue;

		if (pmde_large(src[i])) {
			if (copy_user_pmde_large(&dst[i], src[i]))
				return -1;
			continue;
		}

		if (!pmde_present(dst[i])) {
			uint64_t pt_phys = alloc_pgtable_before_buddy();
			if (!pt_phys)
				return -1;
			pmde_set_pt(dst + i, pt_phys);
		}

		if (copy_user_pt(pmde_to_pt_virt(dst[i]),
				 pmde_to_pt_virt(src[i])))
			return -1;
	}

	return 0;
}

static int copy_user_pgde_large(pgde_t *dst, pgde_t src)
{
	struct page *page = page_alloc(page_order(PAGE_SIZE_1G));
	if (!page)
		return -1;
	uint64_t dst_page = page_to_phys(page);
	uint64_t src_page = pgde_get_large(src);
	memcpy((void *)phys_to_virt(dst_page), (void *)phys_to_virt(src_page),
	       PAGE_SIZE_1G);
	pgde_set_large(dst, dst_page, pgde_flags(src));
	return 0;
}

static int copy_user_pgd(pgde_t *dst, pgde_t *src)
{
	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		if (!pgde_present(src[i]) ||
		    pgde_val(src[i]) == pgde_val(kernel_pgdir[i]))
			continue;

		if (pgde_large(src[i])) {
			if (copy_user_pgde_large(&dst[i], src[i]))
				goto failed;
			continue;
		}

		if (!pgde_present(dst[i])) {
			uint64_t pmd_paddr = alloc_pgtable_before_buddy();
			if (!pmd_paddr)
				goto failed;
			pgde_set_pmd(dst + i, pmd_paddr);
		}

		if (copy_user_pmd(pgde_to_pmd_virt(dst[i]),
				  pgde_to_pmd_virt(src[i])))
			goto failed;
	}

failed:
	destroy_user_pgtable(dst);
	return -1;
}

int copy_user_pgtable(pgde_t *dst, pgde_t *src)
{
	return copy_user_pgd(dst, src);
}

static void cleanup_user_pt(pte_t *pt)
{
	for (size_t j = 0; j < PTRS_PER_PT; ++j) {
		if (!pte_present(pt[j]))
			continue;

		page_free(phys_to_page(pte_get(pt[j])), 0);
	}
}

static void cleanup_user_pmd(pmde_t *pmd)
{
	for (size_t j = 0; j < PTRS_PER_PMD; ++j) {
		if (!pmde_present(pmd[j]))
			continue;

		if (pmde_large(pmd[j])) {
			page_free(phys_to_page(pmde_get_large(pmd[j])),
				   page_order(PAGE_SIZE_2M));
			continue;
		}

		cleanup_user_pt(pmde_to_pt_virt(pmd[j]));

		page_free(phys_to_page(pmde_get_pt(pmd[j])), 0);
	}
}

static void cleanup_user_pgd(pgde_t *pgd)
{
	for (size_t i = 0; i < PTRS_PER_PGD; ++i) {
		if (!pgde_present(pgd[i]) ||
		    pgde_val(pgd[i]) == pgde_val(kernel_pgdir[i]))
			continue;

		if (pgde_large(pgd[i])) {
			page_free(phys_to_page(pgde_get_large(pgd[i])),
				   page_order(PAGE_SIZE_1G));
			continue;
		}

		cleanup_user_pmd(pgde_to_pmd_virt(pgd[i]));

		page_free(phys_to_page(pgde_get_pmd(pgd[i])), 0);
	}
}

void destroy_user_pgtable(pgde_t *pgd)
{
	cleanup_user_pgd(pgd);
	page_free(virt_to_page((uint64_t)pgd), 0);
}
