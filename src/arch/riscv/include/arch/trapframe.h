#ifndef ARCH_TRAPFRAME_H
#define ARCH_TRAPFRAME_H

#include <arch/asm/trap.h>
#include <brk/panic.h>
#include <brk/static_assert.h>
#include <brk/string.h>
#include <brk/types.h>

struct task;

struct riscv_trap_frame {
	union {
		unsigned long x[32];
		/* The following registers are the 32 general-purpose registers
		 * of the RISC-V architecture */
		/* 0   */ unsigned long x0;
		/* 8   */ unsigned long ra;
		/* 16  */ unsigned long sp;
		/* 24  */ unsigned long gp;
		/* 32  */ unsigned long tp;
		/* 40  */ unsigned long t0;
		/* 48  */ unsigned long t1;
		/* 56  */ unsigned long t2;
		/* 64  */ unsigned long s0;
		/* 72  */ unsigned long s1;
		/* 80  */ unsigned long a0;
		/* 88  */ unsigned long a1;
		/* 96  */ unsigned long a2;
		/* 104 */ unsigned long a3;
		/* 112 */ unsigned long a4;
		/* 120 */ unsigned long a5;
		/* 128 */ unsigned long a6;
		/* 136 */ unsigned long a7;
		/* 144 */ unsigned long s2;
		/* 152 */ unsigned long s3;
		/* 160 */ unsigned long s4;
		/* 168 */ unsigned long s5;
		/* 176 */ unsigned long s6;
		/* 184 */ unsigned long s7;
		/* 192 */ unsigned long s8;
		/* 200 */ unsigned long s9;
		/* 208 */ unsigned long s10;
		/* 216 */ unsigned long s11;
		/* 224 */ unsigned long t3;
		/* 232 */ unsigned long t4;
		/* 240 */ unsigned long t5;
		/* 248 */ unsigned long t6;
	};

	/* The following registers are used for trap handling */
	/* 256 */ unsigned long sstatus;
	/* 264 */ unsigned long sepc;
	/* 272 */ unsigned long scause;
	/* 280 */ unsigned long stval;
};

#define TRAP_FRAME_SIZE sizeof(struct riscv_trap_frame)

static_assert(sizeof(struct riscv_trap_frame) == RISCV_TRAP_FRAME_SIZE,
	      "'struct riscv_trap_frame' size mismatch");

#endif
