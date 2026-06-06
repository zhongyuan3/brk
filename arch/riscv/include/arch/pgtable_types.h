#ifndef ARCH_PGTABLE_TYPES_H
#define ARCH_PGTABLE_TYPES_H

#include <brk/types.h>

typedef struct __pgde {
	u64 pgde;
} pgde_t;

typedef struct __pmde {
	u64 pmde;
} pmde_t;

typedef struct __pte {
	u64 pte;
} pte_t;

#endif
