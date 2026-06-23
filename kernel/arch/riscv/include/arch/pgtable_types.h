#ifndef ARCH_PGTABLE_TYPES_H
#define ARCH_PGTABLE_TYPES_H

#include <brk/types.h>

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
