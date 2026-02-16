#ifndef AOSD_TRAP_H
#define AOSD_TRAP_H

void early_trap_vector(void);

void trap_init(uint32_t hart_id);
void kernel_trap_handler(void);
void kernel_trap_vector(void);

#endif
