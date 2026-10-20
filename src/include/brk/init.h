#ifndef BRK_INIT_H
#define BRK_INIT_H

#include <arch/smp.h>

void arch_init(void);

void smp_boot_release_secondary_harts(void);

#endif
