#include <arch/trapframe.h>
#include <brk/string.h>
#include <brk/thread_types.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>

/*
 * The user-visible signal frame is part of the architecture ABI: it
 * contains a complete trap frame.  Keep its layout out of generic code.
 */
struct user_sigframe {
	struct trap_frame tf;
	u64 blocked;
	int signo;
	int pad;
};

unsigned long arch_frame_size(void)
{
	return sizeof(struct trap_frame);
}

void arch_frame_init(struct trap_frame *frame)
{
	memset(frame, 0, sizeof(*frame));
}

void arch_frame_copy(struct trap_frame *dst, const struct trap_frame *src)
{
	memcpy(dst, src, sizeof(*dst));
}

void arch_frame_reset_user_fp(struct trap_frame *frame)
{
	memset(frame->f, 0, sizeof(frame->f));
	frame->fcsr = 0;
}

void arch_frame_set_ret(struct trap_frame *frame, long value)
{
	frame->a0 = (unsigned long)value;
}

void arch_frame_set_kernel_tp(struct trap_frame *frame, unsigned long value)
{
	frame->kernel_tp = value;
}

void arch_setup_user_entry(struct trap_frame *frame, user_addr_t pc,
			   user_addr_t sp, int argc, user_addr_t argv,
			   user_addr_t envp)
{
	frame->sepc = pc;
	frame->sp = sp;
	frame->a0 = (unsigned long)argc;
	frame->a1 = (unsigned long)argv;
	frame->a2 = (unsigned long)envp;
}

void arch_setup_user_thread(struct trap_frame *frame,
			    const struct trap_frame *parent, user_addr_t entry,
			    user_addr_t stack, user_addr_t arg,
			    user_addr_t tls)
{
	frame->sp = stack;
	frame->sepc = entry;
	frame->a0 = arg;
	frame->gp = parent->gp;
	frame->tp = tls ? tls : parent->tp;
}

bool arch_setup_signal_frame(struct thread *thread, int sig,
			     user_addr_t handler)
{
	struct trap_frame *regs = thread->frame;
	user_addr_t sp;
	struct user_sigframe *frame;

	sp = regs->sp - sizeof(*frame);
	sp &= ~0xFULL;
	if (!user_range_ok(sp, sizeof(*frame)))
		return false;

	frame = (struct user_sigframe *)(uintptr_t)sp;
	memcpy(&frame->tf, regs, sizeof(*regs));
	frame->blocked = thread->blocked;
	frame->signo = sig;

	thread->sigframe_sp = sp;
	regs->sp = sp;
	regs->sepc = handler;
	regs->a0 = (unsigned long)sig;
	return true;
}

int arch_restore_signal_frame(struct thread *thread, u64 *ret)
{
	struct user_sigframe *frame;

	if (!thread->sigframe_sp)
		return -EINVAL;

	frame = (struct user_sigframe *)(uintptr_t)thread->sigframe_sp;
	if (!user_range_ok(thread->sigframe_sp, sizeof(*frame)))
		return -EFAULT;

	memcpy(thread->frame, &frame->tf, sizeof(*thread->frame));
	thread->blocked = frame->blocked;
	thread->sigframe_sp = 0;
	*ret = thread->frame->a0;
	return 0;
}
