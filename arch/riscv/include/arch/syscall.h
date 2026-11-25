#ifndef ARCH_SYSCALL_H
#define ARCH_SYSCALL_H

#include <arch/trapframe.h>
#include <brk/panic.h>
#include <brk/types.h>

/*
 * Architecture syscall ABI.
 *
 * Generic code sees a syscall as a number plus an array of logical
 * arguments.  The architecture maps those logical indexes to its own
 * register convention.  Do not expose a0/a1/... to generic code.
 */

static inline u64 arch_syscall_nr(const struct trap_frame *regs)
{
	return regs->a7;
}

static inline u64 arch_syscall_arg(const struct trap_frame *regs,
				   unsigned int argno)
{
	switch (argno) {
	case 0:
		return regs->a0;
	case 1:
		return regs->a1;
	case 2:
		return regs->a2;
	case 3:
		return regs->a3;
	case 4:
		return regs->a4;
	case 5:
		return regs->a5;
	default:
		panic("%s(): illegal argument number: %u\n", __func__, argno);
	}
}

static inline void arch_syscall_set_arg(struct trap_frame *regs,
					unsigned int argno, u64 value)
{
	switch (argno) {
	case 0:
		regs->a0 = value;
		break;
	case 1:
		regs->a1 = value;
		break;
	case 2:
		regs->a2 = value;
		break;
	case 3:
		regs->a3 = value;
		break;
	case 4:
		regs->a4 = value;
		break;
	case 5:
		regs->a5 = value;
		break;
	default:
		panic("%s(): illegal argument number: %u\n", __func__, argno);
	}
}

static inline void arch_syscall_set_ret(struct trap_frame *regs, long value)
{
	regs->a0 = (unsigned long)value;
}

static inline void arch_syscall_set_ret64(struct trap_frame *regs, s64 value)
{
	regs->a0 = (unsigned long)value;
}

static inline void arch_syscall_advance_pc(struct trap_frame *regs)
{
	regs->sepc += 4;
}

static inline void arch_syscall_restart(struct trap_frame *regs)
{
	regs->sepc -= 4;
}

#endif
