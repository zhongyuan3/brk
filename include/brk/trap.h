#ifndef BRK_TRAP_H
#define BRK_TRAP_H

#include <brk/compiler_attr.h>
#include <brk/types.h>
#include <stdbool.h>

struct trap_frame;
struct thread;

/* architecture-specific */

void trap_init(void);

void prepare_to_ret_to_user(struct trap_frame *frame);

void ret_from_fork(void);
void ret_from_user_init(void);

/*
 * Architecture user-context operations.  Generic code uses these instead
 * of reading or writing trap-frame registers directly.
 */
unsigned long arch_frame_size(void);
void arch_frame_init(struct trap_frame *frame);
void arch_frame_copy(struct trap_frame *dst, const struct trap_frame *src);
void arch_frame_reset_user_fp(struct trap_frame *frame);
void arch_frame_set_ret(struct trap_frame *frame, long value);
void arch_frame_set_kernel_tp(struct trap_frame *frame, unsigned long value);

void arch_setup_user_entry(struct trap_frame *frame, user_addr_t pc,
			   user_addr_t sp, int argc, user_addr_t argv,
			   user_addr_t envp);
void arch_setup_user_thread(struct trap_frame *frame,
			    const struct trap_frame *parent, user_addr_t entry,
			    user_addr_t stack, user_addr_t arg,
			    user_addr_t tls);

bool arch_setup_signal_frame(struct thread *thread, int sig,
			     user_addr_t handler);
int arch_restore_signal_frame(struct thread *thread, u64 *ret);

#endif
