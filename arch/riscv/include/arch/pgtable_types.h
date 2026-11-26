#ifndef _ARCH_PGTABLE_TYPES_H
#define _ARCH_PGTABLE_TYPES_H

#include <brk/types.h>

/*
 * RV64 page table entries.  All levels use the same 64-bit encoding; the
 * PGTABLE_LEVELS configuration decides which types are folded.
 */
typedef struct {
	u64 val;
} pgd_t;
#define pgd_val(x) ((x).val)

typedef struct {
	u64 val;
} p4d_t;
#define p4d_val(x) ((x).val)

typedef struct {
	u64 val;
} pud_t;
#define pud_val(x) ((x).val)

typedef struct {
	u64 val;
} pmd_t;
#define pmd_val(x) ((x).val)

typedef struct {
	u64 val;
} pte_t;
#define pte_val(x) ((x).val)

#endif
