#ifndef ARCH_ASM_TRAP_H
#define ARCH_ASM_TRAP_H

/*
 * Hardware saves 32 general-purpose registers (256 bytes) plus sstatus,
 * sepc, scause and stval (32 bytes). The remaining 16 bytes hold software
 * state that must survive while running in user mode: the kernel per-CPU
 * pointer restored on the next user -> kernel transition.
 */
#define RISCV_TRAP_FRAME_SIZE 304
#define RISCV_TRAP_FRAME_KERNEL_TP 288

#endif
