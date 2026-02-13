#ifndef AOSD_TRAP_H
#define AOSD_TRAP_H

void early_trap_vector(void);

void trap_init(void);
void kernel_trap_handler(void);
void kernel_trap_vector(void);

#endif
