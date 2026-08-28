#ifndef ARCH_TRAP_H
#define ARCH_TRAP_H

#include <brk/base/types.h>

#define TRAP_IS_INTERRUPT(scause) ((scause) & (1ULL << 63))
#define TRAP_IS_EXCEPTION(scause) (!TRAP_IS_INTERRUPT(scause))
#define TRAP_CAUSE_CODE(scause) ((scause) & ~(1ULL << 63))

#endif
