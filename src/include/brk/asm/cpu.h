#ifndef BRK_ASM_CPU_H
#define BRK_ASM_CPU_H

#include <arch/asm/page.h>

#ifndef MAX_CPUS
#define MAX_CPUS 8
#endif

#define INIT_STACK_PAGE_ORDER 2
#define NR_INIT_STACK_RSVD_PAGES (MAX_CPUS * (1 << INIT_STACK_PAGE_ORDER))

#endif
