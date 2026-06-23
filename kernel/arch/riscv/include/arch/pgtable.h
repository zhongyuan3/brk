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
#define PTRS_PER_PGD (PAGE_SIZE / sizeof(pgde_t))
#define PGDE_FLAGS_MASK 0x3ff
#define pgde_val(x) ((x).pgde)

static inline unsigned int pgde_flags(pgde_t pgde)
{
	return pgde_val(pgde) & PGDE_FLAGS_MASK;
}

static inline bool pgde_present(pgde_t pgde)
{
	return pgde_flags(pgde) & PTE_V;
}

static inline bool pgde_large(pgde_t pgde)
{
	return pgde_present(pgde) && pgde_flags(pgde) != PTE_V;
}

static inline size_t pgde_index(uint64_t vaddr)
{
	return (vaddr >> PGD_SHIFT) & (PTRS_PER_PGD - 1);
}

static inline void pgde_set_pmd(pgde_t *pgdep, uint64_t pmd_phys)
{
	pgde_val(*pgdep) = ((pmd_phys >> PAGE_SHIFT) << 10) | PTE_V;
}

static inline void pgde_set_large(pgde_t *pgdep, uint64_t phys,
				  unsigned int flags)
{
	pgde_val(*pgdep) = ((phys >> PGD_SHIFT) << 28) | PTE_V | flags;
}

static inline uint64_t pgde_get_pmd(pgde_t pgde)
{
	return (pgde_val(pgde) >> 10) << PAGE_SHIFT;
}

static inline uint64_t pgde_get_large(pgde_t pgde)
{
	return (pgde_val(pgde) >> 28) << PGD_SHIFT;
}

static inline void pgde_clear(pgde_t *pgdep)
{
	pgde_val(*pgdep) = 0;
}

#define PMDE_SHIFT 21
/* Number of entries in the page middle directory */
#define PTRS_PER_PMD (PAGE_SIZE / sizeof(pmde_t))
#define PMDE_FLAGS_MASK 0x3ff
#define pmde_val(x) ((x).pmde)

static inline unsigned int pmde_flags(pmde_t pmde)
{
	return pmde_val(pmde) & PMDE_FLAGS_MASK;
}

static inline bool pmde_present(pmde_t pmde)
{
	return pmde_flags(pmde) & PTE_V;
}

static inline bool pmde_large(pmde_t pmde)
{
	return pmde_present(pmde) && pmde_flags(pmde) != PTE_V;
}

static inline size_t pmde_index(uint64_t vaddr)
{
	return (vaddr >> PMDE_SHIFT) & (PTRS_PER_PMD - 1);
}

static inline void pmde_set_pt(pmde_t *pmdep, uint64_t pt_phys)
{
	pmde_val(*pmdep) = ((pt_phys >> PAGE_SHIFT) << 10) | PTE_V;
}

static inline void pmde_set_large(pmde_t *pmdep, uint64_t phys,
				  unsigned int flags)
{
	pmde_val(*pmdep) = ((phys >> PMDE_SHIFT) << 19) | PTE_V | flags;
}

static inline uint64_t pmde_get_pt(pmde_t pmde)
{
	return (pmde_val(pmde) >> 10) << PAGE_SHIFT;
}

static inline uint64_t pmde_get_large(pmde_t pmde)
{
	return (pmde_val(pmde) >> 19) << PMDE_SHIFT;
}

static inline void pmde_clear(pmde_t *pmdep)
{
	pmde_val(*pmdep) = 0;
}

#define PTE_SHIFT 12
/* Number of entries in the page table */
#define PTRS_PER_PT (PAGE_SIZE / sizeof(pte_t))
#define PTE_FLAGS_MASK 0x3ff
#define pte_val(x) ((x).pte)

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

extern pgde_t early_pgdir[];
extern pgde_t kernel_pgdir[];
extern spinlock_t kernel_pgdir_lock;

pgde_t *create_user_pgtable(void);
void destroy_user_pgtable(pgde_t *pgd);
int copy_user_pgtable(pgde_t *dst, pgde_t *src);

#endif
