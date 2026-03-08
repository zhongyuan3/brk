#ifndef AOSD_TRAP_H
#define AOSD_TRAP_H

#include <aosd/sched_types.h>
#include <aosd/types.h>

#define TRAP_IS_INTERRUPT(scause) ((scause) & (1ULL << 63))
#define TRAP_IS_EXCEPTION(scause) (!TRAP_IS_INTERRUPT(scause))
#define TRAP_CAUSE_CODE(scause) ((scause) & ~(1ULL << 63))

void trap_init(void);
void trap_init_hart(uint32_t hart_id);
void kernel_trap_handler(void);
void kernel_trap_vector(void);

struct task *user_trap_handler(void);
void user_trap_vector(void);
void user_trap_return(struct task *task);
void prepare_to_return(void);

#endif
