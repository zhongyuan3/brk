#ifndef ARCH_PGTABLE_H
#define ARCH_PGTABLE_H

#include <arch/mm.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

#define PTE_V 1 /* Valid */
#define PTE_R 2 /* Read */
#define PTE_W 4 /* Write */
#define PTE_X 8 /* Execute */
#define PTE_U 16 /* User */
#define PTE_G 32 /* Global*/
#define PTE_A 64 /* Accessed */
#define PTE_D 128 /* Dirty */

#define PGD_SHIFT 30
/* Number of entries in the page global directory */
#define PTRS_PER_PGD (PAGE_SIZE / sizeof(pgd_t))
#define PGD_FLAGS_MASK 0x3ff

static inline unsigned int pgd_flags(pgd_t pgd)
{
	return pgd_val(pgd) & PGD_FLAGS_MASK;
}

static inline bool pgd_present(pgd_t pgd)
{
	return pgd_flags(pgd) & PTE_V;
}

static inline bool pgd_large(pgd_t pgd)
{
	return pgd_present(pgd) && pgd_flags(pgd) != PTE_V;
}

static inline size_t pgd_index(uint64_t vaddr)
{
	return (vaddr >> PGD_SHIFT) & (PTRS_PER_PGD - 1);
}

static inline void pgd_set_pmd(pgd_t *pgdp, uint64_t pmd_phys)
{
	pgd_val(*pgdp) = ((pmd_phys >> PAGE_SHIFT) << 10) | PTE_V;
}

static inline void pgd_set_large(pgd_t *pgdp, uint64_t phys, unsigned int flags)
{
	pgd_val(*pgdp) = ((phys >> PGD_SHIFT) << 28) | PTE_V | flags;
}

static inline uint64_t pgd_get_pmd(pgd_t pgd)
{
	return (pgd_val(pgd) >> 10) << PAGE_SHIFT;
}

static inline uint64_t pgd_get_large(pgd_t pgd)
{
	return (pgd_val(pgd) >> 28) << PGD_SHIFT;
}

static inline void pgd_clear(pgd_t *pgdp)
{
	pgd_val(*pgdp) = 0;
}

#define PMD_SHIFT 21
/* Number of entries in the page middle directory */
#define PTRS_PER_PMD (PAGE_SIZE / sizeof(pmd_t))
#define PMD_FLAGS_MASK 0x3ff

static inline unsigned int pmd_flags(pmd_t pmd)
{
	return pmd_val(pmd) & PMD_FLAGS_MASK;
}

static inline bool pmd_present(pmd_t pmd)
{
	return pmd_flags(pmd) & PTE_V;
}

static inline bool pmd_large(pmd_t pmd)
{
	return pmd_present(pmd) && pmd_flags(pmd) != PTE_V;
}

static inline size_t pmd_index(uint64_t vaddr)
{
	return (vaddr >> PMD_SHIFT) & (PTRS_PER_PMD - 1);
}

static inline void pmd_set_pte(pmd_t *pmdp, uint64_t pt_phys)
{
	pmd_val(*pmdp) = ((pt_phys >> PAGE_SHIFT) << 10) | PTE_V;
}

static inline void pmd_set_large(pmd_t *pmdp, uint64_t phys, unsigned int flags)
{
	pmd_val(*pmdp) = ((phys >> PMD_SHIFT) << 19) | PTE_V | flags;
}

static inline uint64_t pmd_get_pte(pmd_t pmd)
{
	return (pmd_val(pmd) >> 10) << PAGE_SHIFT;
}

static inline uint64_t pmd_get_large(pmd_t pmd)
{
	return (pmd_val(pmd) >> 19) << PMD_SHIFT;
}

static inline void pmd_clear(pmd_t *pmdp)
{
	pmd_val(*pmdp) = 0;
}

#define PTE_SHIFT 12
/* Number of entries in the page table */
#define PTRS_PER_PT (PAGE_SIZE / sizeof(pte_t))
#define PTE_FLAGS_MASK 0x3ff

static inline unsigned int pte_flags(pte_t pte)
{
	return pte_val(pte) & PTE_FLAGS_MASK;
}

static inline bool pte_present(pte_t pte)
{
	return pte_flags(pte) & PTE_V;
}

static inline size_t pte_index(uint64_t vaddr)
{
	return (vaddr >> PTE_SHIFT) & (PTRS_PER_PT - 1);
}

static inline void pte_set(pte_t *ptep, uint64_t phys, unsigned int flags)
{
	pte_val(*ptep) = ((phys >> PAGE_SHIFT) << 10) | PTE_V | flags;
}

static inline uint64_t pte_get(pte_t pte)
{
	return (pte_val(pte) >> 10) << PAGE_SHIFT;
}

static inline void pte_clear(pte_t *ptep)
{
	pte_val(*ptep) = 0;
}

extern pgd_t early_pgdir[];
extern pgd_t kernel_pgdir[];
extern spinlock_t kernel_pgdir_lock;

pgd_t *create_user_pgtable(void);
void destroy_user_pgtable(pgd_t *pgd);
int copy_user_pgtable(pgd_t *dst, pgd_t *src);

#endif
