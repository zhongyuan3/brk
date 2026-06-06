#ifndef BRK_TRAP_H
#define BRK_TRAP_H

#include <brk/task.h>
#include <brk/types.h>

void trap_init_hart(u32 hart_id);
void kernel_trap_handler(void);
void kernel_trap_vector(void);

struct trap_frame *user_trap_handler(void);
void user_trap_vector(void);
void user_trap_return(struct trap_frame *tf);
void prepare_to_return(void);

#endif
