#ifndef ARCH_PGTABLE_TYPES_H
#define ARCH_PGTABLE_TYPES_H

#include <brk/types.h>

/*
 * RV64/SV39: three-level page table (PGD -> PMD -> PTE), 64-bit entries.
 */
typedef struct {
	u64 pgd;
} pgd_t;
#define pgd_val(x) ((x).pgd)

typedef struct {
	u64 pmd;
} pmd_t;
#define pmd_val(x) ((x).pmd)

typedef struct {
	u64 pte;
} pte_t;
#define pte_val(x) ((x).pte)

#endif
