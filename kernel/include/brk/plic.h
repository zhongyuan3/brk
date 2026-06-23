#ifndef BRK_PLIC_H
#define BRK_PLIC_H

#include <brk/types.h>

#define PLIC_PRIORITY_OFFSET 0x000000
#define PLIC_PENDING_OFFSET 0x001000
#define PLIC_ENABLE_OFFSET 0x002000
#define PLIC_THRESHOLD_OFFSET 0x200000
#define PLIC_CLAIM_COMPLETE_OFFSET 0x200004

struct plic_device {
	uint64_t phys_base;
	size_t size;
	uint32_t ndev;
};

void plic_init(void);
int plic_enable(uint32_t hart_id, uint32_t source);
int plic_disable(uint32_t hart_id, uint32_t source);
int plic_set_priority(uint32_t source, unsigned int priority);
int plic_set_threshold(uint32_t hart_id, unsigned int threshold);
int plic_claim(uint32_t hart_id, uint32_t *source);
int plic_complete(uint32_t hart_id, uint32_t source);
uint32_t plic_get_ndev(void);

#endif
