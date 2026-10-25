#ifndef BRK_PLIC_H
#define BRK_PLIC_H

#include <brk/irq.h>
#include <brk/types.h>

struct plic_device {
	u64 phys_base;
	size_t size;
	u32 ndev;
	void *client_data;
	struct irq_chip *chip;
};

#endif