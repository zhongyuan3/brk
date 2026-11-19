#ifndef ARCH_ASM_TRAP_H
#define ARCH_ASM_TRAP_H

/*
 * Hardware saves 32 general-purpose registers (256 bytes) plus sstatus,
 * sepc, scause and stval (32 bytes). The next 16 bytes hold software state
 * that must survive while running in user mode: the kernel per-CPU pointer
 * restored on the next user -> kernel transition.
 *
 * The following 264 bytes hold the user floating-point context: f0-f31
 * (256 bytes) and fcsr (8 bytes). trap_entry saves them on every user ->
 * kernel transition and ret_from_trap restores them when returning to user
 * mode, so preemptive scheduling preserves the FP registers across context
 * switches. Kernel-mode traps do not touch the FP area: the kernel contains
 * no floating-point code.
 */
#define __RISCV_TRAP_FRAME_SIZE 576
#define __RISCV_TRAP_FRAME_KERNEL_TP 288
#define __RISCV_TRAP_FRAME_FP 304
#define __RISCV_TRAP_FRAME_FCSR 560

#endif
