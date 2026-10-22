#ifndef ARCH_TRAPFRAME_H
#define ARCH_TRAPFRAME_H

#include <brk/panic.h>
#include <brk/string.h>
#include <brk/types.h>

struct task;

struct trap_frame {
	/* 0   */ unsigned long kernel_sp;
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
	/* 256 */ unsigned long epc;
	/* 264 */ unsigned long cpuid;
};

struct extended_trap_frame {
	struct trap_frame tf;
	struct task *task;
};

#define TRAP_FRAME_ON_STACK_SIZE \
	round_up_pow2_const(sizeof(struct extended_trap_frame), 16)

#define ARCH_TF_SYSCALL_INSN_SIZE 4

static inline void arch_tf_init(struct trap_frame *tf)
{
	memset(tf, 0, sizeof(*tf));
}

static inline void arch_tf_copy(struct trap_frame *dst,
				const struct trap_frame *src)
{
	memcpy(dst, src, sizeof(*dst));
}

static inline u64 arch_tf_get_sp(const struct trap_frame *tf)
{
	return tf->sp;
}

static inline void arch_tf_set_sp(struct trap_frame *tf, u64 sp)
{
	tf->sp = sp;
}

static inline u64 arch_tf_get_pc(const struct trap_frame *tf)
{
	return tf->epc;
}

static inline void arch_tf_set_pc(struct trap_frame *tf, u64 pc)
{
	tf->epc = pc;
}

static inline void arch_tf_advance_pc(struct trap_frame *tf, u64 delta)
{
	tf->epc += delta;
}

static inline void arch_tf_skip_syscall(struct trap_frame *tf)
{
	arch_tf_advance_pc(tf, ARCH_TF_SYSCALL_INSN_SIZE);
}

static inline void arch_tf_set_user_entry(struct trap_frame *tf, u64 pc, u64 sp)
{
	arch_tf_set_pc(tf, pc);
	arch_tf_set_sp(tf, sp);
}

static inline u64 arch_tf_get_a0(const struct trap_frame *tf)
{
	return tf->a0;
}

static inline void arch_tf_set_a0(struct trap_frame *tf, u64 val)
{
	tf->a0 = val;
}

static inline void arch_tf_set_a1(struct trap_frame *tf, u64 val)
{
	tf->a1 = val;
}

static inline void arch_tf_set_a2(struct trap_frame *tf, u64 val)
{
	tf->a2 = val;
}

static inline u64 arch_tf_get_syscall_nr(const struct trap_frame *tf)
{
	return tf->a7;
}

static inline u64 arch_tf_get_arg(const struct trap_frame *tf, int argno)
{
	switch (argno) {
	case 0:
		return tf->a0;
	case 1:
		return tf->a1;
	case 2:
		return tf->a2;
	case 3:
		return tf->a3;
	case 4:
		return tf->a4;
	case 5:
		return tf->a5;
	default:
		panic("%s(): illegal argument number\n", __func__);
	}
}

static inline void arch_tf_set_kernel_sp(struct trap_frame *tf, u64 sp)
{
	tf->kernel_sp = sp;
}

static inline cpuid_t arch_tf_get_cpuid(const struct trap_frame *tf)
{
	return tf->cpuid;
}

static inline void arch_tf_set_cpuid(struct trap_frame *tf, cpuid_t cpuid)
{
	tf->cpuid = cpuid;
}

#endif
