#ifndef _BRK_INIT_H
#define _BRK_INIT_H

#include <brk/compiler_attr.h>
void start_kernel(void);

/* architecture-specific */

void arch_init(void);
void bring_up_secondaries(void);

#endif
