#ifndef BRK_INIT_H
#define BRK_INIT_H

#include <asm/smp.h>

void arch_init(void);

#if ENABLE_SMP
void smp_boot_release_secondary_harts(void);
#endif

#endif
