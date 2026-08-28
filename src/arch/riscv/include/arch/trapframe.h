#ifndef ARCH_TRAPFRAME_H
#define ARCH_TRAPFRAME_H

#include <arch/processor.h>
#include <brk/base/types.h>
#include <brk/lib/string.h>
#include <brk/printk/panic.h>

struct task_control_block;

struct trap_frame {
	/* 0   */ reg_t kernel_sp;
	/* 8   */ reg_t ra;
	/* 16  */ reg_t sp;
	/* 24  */ reg_t gp;
	/* 32  */ reg_t tp;
	/* 40  */ reg_t t0;
	/* 48  */ reg_t t1;
	/* 56  */ reg_t t2;
	/* 64  */ reg_t s0;
	/* 72  */ reg_t s1;
	/* 80  */ reg_t a0;
	/* 88  */ reg_t a1;
	/* 96  */ reg_t a2;
	/* 104 */ reg_t a3;
	/* 112 */ reg_t a4;
	/* 120 */ reg_t a5;
	/* 128 */ reg_t a6;
	/* 136 */ reg_t a7;
	/* 144 */ reg_t s2;
	/* 152 */ reg_t s3;
	/* 160 */ reg_t s4;
	/* 168 */ reg_t s5;
	/* 176 */ reg_t s6;
	/* 184 */ reg_t s7;
	/* 192 */ reg_t s8;
	/* 200 */ reg_t s9;
	/* 208 */ reg_t s10;
	/* 216 */ reg_t s11;
	/* 224 */ reg_t t3;
	/* 232 */ reg_t t4;
	/* 240 */ reg_t t5;
	/* 248 */ reg_t t6;
	/* 256 */ reg_t epc;
	/* 264 */ reg_t cpuid;
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

static inline uint64_t arch_tf_get_sp(const struct trap_frame *tf)
{
	return tf->sp;
}

static inline void arch_tf_set_sp(struct trap_frame *tf, uint64_t sp)
{
	tf->sp = sp;
}

static inline uint64_t arch_tf_get_pc(const struct trap_frame *tf)
{
	return tf->epc;
}

static inline void arch_tf_set_pc(struct trap_frame *tf, uint64_t pc)
{
	tf->epc = pc;
}

static inline void arch_tf_advance_pc(struct trap_frame *tf, uint64_t delta)
{
	tf->epc += delta;
}

static inline void arch_tf_skip_syscall(struct trap_frame *tf)
{
	arch_tf_advance_pc(tf, ARCH_TF_SYSCALL_INSN_SIZE);
}

static inline void arch_tf_set_user_entry(struct trap_frame *tf, uint64_t pc,
					  uint64_t sp)
{
	arch_tf_set_pc(tf, pc);
	arch_tf_set_sp(tf, sp);
}

static inline uint64_t arch_tf_get_a0(const struct trap_frame *tf)
{
	return tf->a0;
}

static inline void arch_tf_set_a0(struct trap_frame *tf, uint64_t val)
{
	tf->a0 = val;
}

static inline void arch_tf_set_a1(struct trap_frame *tf, uint64_t val)
{
	tf->a1 = val;
}

static inline void arch_tf_set_a2(struct trap_frame *tf, uint64_t val)
{
	tf->a2 = val;
}

static inline uint64_t arch_tf_get_syscall_nr(const struct trap_frame *tf)
{
	return tf->a7;
}

static inline uint64_t arch_tf_get_arg(const struct trap_frame *tf, int argno)
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

static inline void arch_tf_set_kernel_sp(struct trap_frame *tf, uint64_t sp)
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
