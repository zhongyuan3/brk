#ifndef ARCH_PGTABLE_TYPES_H
#define ARCH_PGTABLE_TYPES_H

#include <brk/base/types.h>

/*
 * RV64/SV39: three-level page table (PGD -> PMD -> PTE), 64-bit entries.
 * RV32/Sv32: two-level page table (PGD -> PTE), 32-bit entries. The PMD
 * level is folded onto the PGD entry (PTRS_PER_PMD == 1), mirroring
 * Linux's __PAGETABLE_PMD_FOLDED.
 */
#if __riscv_xlen == 32

typedef struct __pgd {
	uint32_t pgd;
} pgd_t;
#define pgd_val(x) ((x).pgd)

typedef struct __pmd {
	uint32_t pmd;
} pmd_t;
#define pmd_val(x) ((x).pmd)

typedef struct __pte {
	uint32_t pte;
} pte_t;
#define pte_val(x) ((x).pte)

#else

typedef struct __pgd {
	uint64_t pgd;
} pgd_t;
#define pgd_val(x) ((x).pgd)

typedef struct __pmd {
	uint64_t pmd;
} pmd_t;
#define pmd_val(x) ((x).pmd)

typedef struct __pte {
	uint64_t pte;
} pte_t;
#define pte_val(x) ((x).pte)

#endif

#endif
