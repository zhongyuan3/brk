#ifndef ARCH_TRAP_H
#define ARCH_TRAP_H

#include <brk/types.h>

/* scause is a 64-bit CSR: the interrupt bit is bit 63. */
#define TRAP_IS_INTERRUPT(scause) ((scause) & (1ULL << 63))
#define TRAP_IS_EXCEPTION(scause) (!TRAP_IS_INTERRUPT(scause))
#define TRAP_CAUSE_CODE(scause) ((scause) & ~(1ULL << 63))

void setup_trap_entry_early(void);
void setup_trap_entry(void);

#endif