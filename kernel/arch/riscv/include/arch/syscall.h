#ifndef ARCH_SYSCALL_H
#define ARCH_SYSCALL_H

#include <arch/trapframe.h>

static inline u64 arch_syscall_get_nr(struct trap_frame *tf)
{
	return arch_tf_get_syscall_nr(tf);
}

static inline void arch_syscall_set_ret(struct trap_frame *tf, u64 val)
{
	arch_tf_set_a0(tf, val);
}

static inline u64 arch_syscall_get_arg(struct trap_frame *tf, int argno)
{
	return arch_tf_get_arg(tf, argno);
}

#endif
