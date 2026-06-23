#ifndef ARCH_PGTABLE_TYPES_H
#define ARCH_PGTABLE_TYPES_H

#include <brk/types.h>

typedef struct __pgde {
	uint64_t pgde;
} pgde_t;

typedef struct __pmde {
	uint64_t pmde;
} pmde_t;

typedef struct __pte {
	uint64_t pte;
} pte_t;

#endif
