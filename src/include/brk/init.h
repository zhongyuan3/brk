#ifndef BRK_INIT_H
#define BRK_INIT_H

void start_kernel(void);

void arch_init(void);

void smp_boot_release_secondary_harts(void);

#endif
