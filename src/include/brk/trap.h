#ifndef BRK_TRAP_H
#define BRK_TRAP_H

struct trap_frame;

/* architecture-specific */

void trap_init(void);

void prepare_to_ret_to_user(struct trap_frame *frame);

void ret_from_fork(void);
void ret_from_user_init(void);

#endif
