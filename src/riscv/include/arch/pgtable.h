#ifndef ARCH_PGTABLE_H
#define ARCH_PGTABLE_H

#include <arch/asm/page.h>
#include <arch/asm/pgtable_config.h>
#include <arch/mm.h>
#include <arch/pgtable_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <stdbool.h>
#include <stddef.h>

#define PTE_V 1 /* Valid */
#define PTE_R 2 /* Read */
#define PTE_W 4 /* Write */
#define PTE_X 8 /* Execute */
#define PTE_U 16 /* User */
#define PTE_G 32 /* Global*/
#define PTE_A 64 /* Accessed */
#define PTE_D 128 /* Dirty */
#define PTE_COW 256 /* Software: page is shared and must be copied on write */

#define PGTABLE_FLAGS_MASK 0x3ff
#define PGD_FLAGS_MASK PGTABLE_FLAGS_MASK
#define P4D_FLAGS_MASK PGTABLE_FLAGS_MASK
#define PUD_FLAGS_MASK PGTABLE_FLAGS_MASK
#define PMD_FLAGS_MASK PGTABLE_FLAGS_MASK
#define PTE_FLAGS_MASK PGTABLE_FLAGS_MASK

#define PTRS_PER_PGD (PAGE_SIZE / sizeof(pgd_t))
#if HAVE_P4D
#define PTRS_PER_P4D (PAGE_SIZE / sizeof(p4d_t))
#else
#define PTRS_PER_P4D 1
#endif
#if HAVE_PUD
#define PTRS_PER_PUD (PAGE_SIZE / sizeof(pud_t))
#else
#define PTRS_PER_PUD 1
#endif
#define PTRS_PER_PMD (PAGE_SIZE / sizeof(pmd_t))
#define PTRS_PER_PT (PAGE_SIZE / sizeof(pte_t))

#define pgd_flags(x) (pgd_val(x) & PGTABLE_FLAGS_MASK)
#define p4d_flags(x) (p4d_val(x) & PGTABLE_FLAGS_MASK)
#define pud_flags(x) (pud_val(x) & PGTABLE_FLAGS_MASK)
#define pmd_flags(x) (pmd_val(x) & PGTABLE_FLAGS_MASK)
#define pte_flags(x) (pte_val(x) & PGTABLE_FLAGS_MASK)

#define pgd_is_valid(x) (pgd_flags(x) & PTE_V)
#define p4d_is_valid(x) (p4d_flags(x) & PTE_V)
#define pud_is_valid(x) (pud_flags(x) & PTE_V)
#define pmd_is_valid(x) (pmd_flags(x) & PTE_V)
#define pte_is_valid(x) (pte_flags(x) & PTE_V)

#define pgd_is_leaf(x) (pgd_is_valid(x) && pgd_flags(x) != PTE_V)
#define p4d_is_leaf(x) (p4d_is_valid(x) && p4d_flags(x) != PTE_V)
#define pud_is_leaf(x) (pud_is_valid(x) && pud_flags(x) != PTE_V)
#define pmd_is_leaf(x) (pmd_is_valid(x) && pmd_flags(x) != PTE_V)

#define pgd_none(x) (!pgd_is_valid(x))
#define p4d_none(x) (!p4d_is_valid(x))
#define pud_none(x) (!pud_is_valid(x))
#define pmd_none(x) (!pmd_is_valid(x))
#define pte_none(x) (!pte_is_valid(x))

#define PGTABLE_NEXT(x) (((x) >> 10) << PAGE_SHIFT)
#define PGTABLE_LEAF(x) (((x) >> 10) << PAGE_SHIFT)
#define PGTABLE_MK(x) (((u64)(x) >> PAGE_SHIFT) << 10)

static inline size_t pgd_index(virt_addr_t vaddr)
{
	return (vaddr >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1);
}

static inline size_t p4d_index(virt_addr_t vaddr)
{
	return (vaddr >> P4D_SHIFT) & (PTRS_PER_P4D - 1);
}

static inline size_t pud_index(virt_addr_t vaddr)
{
	return (vaddr >> PUD_SHIFT) & (PTRS_PER_PUD - 1);
}

static inline size_t pmd_index(virt_addr_t vaddr)
{
	return (vaddr >> PMD_SHIFT) & (PTRS_PER_PMD - 1);
}

static inline size_t pte_index(virt_addr_t vaddr)
{
	return (vaddr >> PTE_SHIFT) & (PTRS_PER_PT - 1);
}

static inline pgd_t *pgd_offset(pgd_t *pgd, virt_addr_t vaddr)
{
	return &pgd[pgd_index(vaddr)];
}

static inline p4d_t *p4d_offset(pgd_t *pgde, u64 vaddr,
				void *(*phys_to_virt)(phys_addr_t))
{
#if HAVE_P4D
	return (p4d_t *)phys_to_virt(PGTABLE_NEXT(pgd_val(*pgde))) +
	       p4d_index(vaddr);
#else
	(void)vaddr;
	(void)phys_to_virt;
	return (p4d_t *)pgde;
#endif
}

static inline pud_t *pud_offset(p4d_t *p4de, u64 vaddr,
				void *(*phys_to_virt)(phys_addr_t))
{
#if HAVE_PUD
	return (pud_t *)phys_to_virt(PGTABLE_NEXT(p4d_val(*p4de))) +
	       pud_index(vaddr);
#else
	(void)vaddr;
	(void)phys_to_virt;
	return (pud_t *)p4de;
#endif
}

static inline pmd_t *pmd_offset(pmd_t *pmd, virt_addr_t vaddr)
{
	return &pmd[pmd_index(vaddr)];
}

static inline pte_t *pte_offset(pte_t *pt, virt_addr_t vaddr)
{
	return &pt[pte_index(vaddr)];
}

static inline void pgd_set_next(pgd_t *pgdp, u64 next_phys)
{
	pgd_val(*pgdp) = PGTABLE_MK(next_phys) | PTE_V;
}

static inline void p4d_set_next(p4d_t *p4dp, u64 next_phys)
{
	p4d_val(*p4dp) = PGTABLE_MK(next_phys) | PTE_V;
}

static inline void pud_set_next(pud_t *pudp, u64 next_phys)
{
	pud_val(*pudp) = PGTABLE_MK(next_phys) | PTE_V;
}

static inline void pmd_set_next(pmd_t *pmdp, u64 next_phys)
{
	pmd_val(*pmdp) = PGTABLE_MK(next_phys) | PTE_V;
}

static inline void pgd_set_leaf(pgd_t *pgdp, u64 phys, unsigned int flags)
{
	pgd_val(*pgdp) = PGTABLE_MK(phys) | PTE_V | flags;
}

static inline void p4d_set_leaf(p4d_t *p4dp, u64 phys, unsigned int flags)
{
	p4d_val(*p4dp) = PGTABLE_MK(phys) | PTE_V | flags;
}

static inline void pud_set_leaf(pud_t *pudp, u64 phys, unsigned int flags)
{
	pud_val(*pudp) = PGTABLE_MK(phys) | PTE_V | flags;
}

static inline void pmd_set_leaf(pmd_t *pmdp, u64 phys, unsigned int flags)
{
	pmd_val(*pmdp) = PGTABLE_MK(phys) | PTE_V | flags;
}

static inline void pte_set_leaf(pte_t *ptep, u64 phys, unsigned int flags)
{
	pte_val(*ptep) = PGTABLE_MK(phys) | PTE_V | flags;
}

static inline u64 pgd_next(pgd_t pgd)
{
	return PGTABLE_NEXT(pgd_val(pgd));
}

static inline u64 p4d_next(p4d_t p4d)
{
	return PGTABLE_NEXT(p4d_val(p4d));
}

static inline u64 pud_next(pud_t pud)
{
	return PGTABLE_NEXT(pud_val(pud));
}

static inline u64 pmd_next(pmd_t pmd)
{
	return PGTABLE_NEXT(pmd_val(pmd));
}

static inline u64 pgd_leaf(pgd_t pgd)
{
	return PGTABLE_LEAF(pgd_val(pgd));
}

static inline u64 p4d_leaf(p4d_t p4d)
{
	return PGTABLE_LEAF(p4d_val(p4d));
}

static inline u64 pud_leaf(pud_t pud)
{
	return PGTABLE_LEAF(pud_val(pud));
}

static inline u64 pmd_leaf(pmd_t pmd)
{
	return PGTABLE_LEAF(pmd_val(pmd));
}

static inline u64 pte_leaf(pte_t pte)
{
	return PGTABLE_LEAF(pte_val(pte));
}

static inline void pgd_clear(pgd_t *pgdp)
{
	pgd_val(*pgdp) = 0;
}

static inline void p4d_clear(p4d_t *p4dp)
{
	p4d_val(*p4dp) = 0;
}

static inline void pud_clear(pud_t *pudp)
{
	pud_val(*pudp) = 0;
}

static inline void pmd_clear(pmd_t *pmdp)
{
	pmd_val(*pmdp) = 0;
}

static inline void pte_clear(pte_t *ptep)
{
	pte_val(*ptep) = 0;
}

static inline pud_t *p4d_to_pud(p4d_t p4de, void *(*phys_to_virt)(phys_addr_t))
{
	return (pud_t *)phys_to_virt(p4d_next(p4de));
}

static inline pmd_t *pud_to_pmd(pud_t pude, void *(*phys_to_virt)(phys_addr_t))
{
	return (pmd_t *)phys_to_virt(pud_next(pude));
}

static inline pte_t *pmd_to_pt(pmd_t pmde, void *(*phys_to_virt)(phys_addr_t))
{
	return (pte_t *)phys_to_virt(pmd_next(pmde));
}

static inline void *pgtable_phys_to_virt(phys_addr_t paddr)
{
	return (void *)(uintptr_t)phys_to_virt(paddr);
}

extern pgd_t kernel_pgdir[];
extern spinlock_t kernel_pgdir_lock;

pgd_t *pgd_alloc_user(void);
void pgd_free_user(pgd_t *pgd);
int pgd_copy_user(pgd_t *dst, pgd_t *src);

#endif
