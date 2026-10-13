#ifndef BRK_PLIC_H
#define BRK_PLIC_H

#include <brk/types.h>

struct plic_device {
	u64 phys_base;
	size_t size;
	u32 ndev;
};

void plic_init(void);

#endif