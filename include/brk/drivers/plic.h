#ifndef BRK_PLIC_H
#define BRK_PLIC_H

#include <brk/lib/types.h>

#define PLIC_PRIORITY_OFFSET 0x000000
#define PLIC_PENDING_OFFSET 0x001000
#define PLIC_ENABLE_OFFSET 0x002000
#define PLIC_THRESHOLD_OFFSET 0x200000
#define PLIC_CLAIM_COMPLETE_OFFSET 0x200004

struct plic_device {
	u64 phys_base;
	usize_t size;
	u32 ndev;
};

void plic_init(void);
int plic_enable(u32 hart_id, u32 source);
int plic_disable(u32 hart_id, u32 source);
int plic_set_priority(u32 source, unsigned int priority);
int plic_set_threshold(u32 hart_id, unsigned int threshold);
int plic_claim(u32 hart_id, u32 *source);
int plic_complete(u32 hart_id, u32 source);
u32 plic_get_ndev(void);

#endif
