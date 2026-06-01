#ifndef BRK_TRAP_FRAME_H
#define BRK_TRAP_FRAME_H

#include <brk/types.h>

struct task_control_block;

struct trap_frame {
	/* 0   */ u64 kernel_sp;
	/* 8   */ u64 ra;
	/* 16  */ u64 sp;
	/* 24  */ u64 gp;
	/* 32  */ u64 tp;
	/* 40  */ u64 t0;
	/* 48  */ u64 t1;
	/* 56  */ u64 t2;
	/* 64  */ u64 s0;
	/* 72  */ u64 s1;
	/* 80  */ u64 a0;
	/* 88  */ u64 a1;
	/* 96  */ u64 a2;
	/* 104 */ u64 a3;
	/* 112 */ u64 a4;
	/* 120 */ u64 a5;
	/* 128 */ u64 a6;
	/* 136 */ u64 a7;
	/* 144 */ u64 s2;
	/* 152 */ u64 s3;
	/* 160 */ u64 s4;
	/* 168 */ u64 s5;
	/* 176 */ u64 s6;
	/* 184 */ u64 s7;
	/* 192 */ u64 s8;
	/* 200 */ u64 s9;
	/* 208 */ u64 s10;
	/* 216 */ u64 s11;
	/* 224 */ u64 t3;
	/* 232 */ u64 t4;
	/* 240 */ u64 t5;
	/* 248 */ u64 t6;
	/* 256 */ u64 epc;
	/* 264 */ u64 cpuid;
};

struct extended_trap_frame {
	struct trap_frame tf;
	struct task_control_block *task;
};

#define TRAP_FRAME_ON_STACK_SIZE \
	round_up_pow2_const(sizeof(struct extended_trap_frame), 16)

#endif
