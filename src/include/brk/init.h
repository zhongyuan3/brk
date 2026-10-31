#ifndef BRK_INIT_H
#define BRK_INIT_H

void start_kernel(void);

/* architecture-specific */

int arch_init(void);
void bring_up_secondaries(void);

#endif
