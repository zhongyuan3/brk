#ifndef ARCH_TRAP_H
#define ARCH_TRAP_H

#include <brk/base/types.h>

/*
 * scause is XLEN-wide: the interrupt bit is bit 63 on RV64 and bit 31
 * on RV32.
 */
#if __riscv_xlen == 32
#define TRAP_IS_INTERRUPT(scause) ((scause) & (1UL << 31))
#define TRAP_IS_EXCEPTION(scause) (!TRAP_IS_INTERRUPT(scause))
#define TRAP_CAUSE_CODE(scause) ((scause) & ~(1UL << 31))
#else
#define TRAP_IS_INTERRUPT(scause) ((scause) & (1ULL << 63))
#define TRAP_IS_EXCEPTION(scause) (!TRAP_IS_INTERRUPT(scause))
#define TRAP_CAUSE_CODE(scause) ((scause) & ~(1ULL << 63))
#endif

#endif