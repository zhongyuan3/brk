#ifndef ARCH_TRAPFRAME_H
#define ARCH_TRAPFRAME_H

#include <brk/kernel/panic.h>
#include <brk/lib/string.h>
#include <brk/lib/types.h>

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
