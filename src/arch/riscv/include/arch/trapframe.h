#ifndef ARCH_TRAPFRAME_H
#define ARCH_TRAPFRAME_H

#include <arch/asm/trap.h>
#include <brk/assert.h>
#include <brk/panic.h>
#include <brk/string.h>
#include <brk/types.h>
#include <stddef.h>

struct trap_frame {
	union {
		unsigned long x[32];
		/* The following registers are the 32 general-purpose registers
		 * of the RISC-V architecture. They must be wrapped in their own
		 * struct: a union only overlays its members, it does not lay
		 * them out sequentially. */
		struct {
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
	};

	/* The following registers are used for trap handling */
	/* 256 */ unsigned long sstatus;
	/* 264 */ unsigned long sepc;
	/* 272 */ unsigned long scause;
	/* 280 */ unsigned long stval;

	/*
	 * Software state for the user <-> kernel transition. ret_from_trap
	 * stores the kernel per-CPU pointer here before restoring the user's
	 * tp, and trap_entry reloads it when entering the kernel from user
	 * mode.
	 */
	/* 288 */ unsigned long kernel_tp;

	/* Keep the frame 16-byte aligned. */
	/* 296 */ unsigned long __pad;
};

#define TRAP_FRAME_SIZE sizeof(struct trap_frame)

static_assert(sizeof(struct trap_frame) == __RISCV_TRAP_FRAME_SIZE,
	      "'struct trap_frame' size mismatch");
static_assert(offsetof(struct trap_frame, sp) == 16,
	      "'struct trap_frame' sp offset mismatch");
static_assert(offsetof(struct trap_frame, a7) == 136,
	      "'struct trap_frame' a7 offset mismatch");
static_assert(offsetof(struct trap_frame, sstatus) == 256,
	      "'struct trap_frame' sstatus offset mismatch");
static_assert(offsetof(struct trap_frame, sepc) == 264,
	      "'struct trap_frame' sepc offset mismatch");
static_assert(offsetof(struct trap_frame, kernel_tp) == 288,
	      "'struct trap_frame' kernel_tp offset mismatch");

static inline void trap_frame_set_kernel_tp(struct trap_frame *frame,
					    unsigned long value)
{
	frame->kernel_tp = value;
}

static inline void trap_frame_set_sp(struct trap_frame *frame,
				     unsigned long value)
{
	frame->sp = value;
}

static inline unsigned long trap_frame_get_sp(struct trap_frame *frame)
{
	return frame->sp;
}

static inline void trap_frame_set_pc(struct trap_frame *frame,
				     unsigned long value)
{
	frame->sepc = value;
}

static inline void trap_frame_set_ret_val(struct trap_frame *frame,
					  unsigned long value)
{
	frame->a0 = value;
}

static inline void trap_frame_set_arg(struct trap_frame *frame, int argno,
				      unsigned long value)
{
	switch (argno) {
	case 0:
		frame->a0 = value;
		break;
	case 1:
		frame->a1 = value;
		break;
	case 2:
		frame->a2 = value;
		break;
	case 3:
		frame->a3 = value;
		break;
	case 4:
		frame->a4 = value;
		break;
	case 5:
		frame->a5 = value;
		break;
	default:
		panic("%s(): illegal argument number: %d\n", __func__, argno);
	}
}

static inline unsigned long trap_frame_get_arg(struct trap_frame *frame,
					       unsigned int argno)
{
	switch (argno) {
	case 0:
		return frame->a0;
	case 1:
		return frame->a1;
	case 2:
		return frame->a2;
	case 3:
		return frame->a3;
	case 4:
		return frame->a4;
	case 5:
		return frame->a5;
	default:
		panic("%s(): illegal argument number: %d\n", __func__, argno);
	}
}

static inline unsigned long trap_frame_get_syscall_no(struct trap_frame *frame)
{
	return frame->a7;
}

#endif
