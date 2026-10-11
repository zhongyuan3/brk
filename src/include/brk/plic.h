#ifndef BRK_PLIC_H
#define BRK_PLIC_H

#include <brk/types.h>

struct plic_device {
	uint64_t phys_base;
	size_t size;
	uint32_t ndev;
};

void plic_init(void);

#endif