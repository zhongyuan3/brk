#ifndef BRK_INIT_H
#define BRK_INIT_H

#include <brk/compiler_attr.h>
void start_kernel(void);

/* architecture-specific */

__result(int) arch_init(void);
void bring_up_secondaries(void);

#endif
