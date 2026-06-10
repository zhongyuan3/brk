#ifndef BRK_CPU_H
#define BRK_CPU_H

#include <brk/lib/types.h>

void cpu_set_timebase_freq(u32 freq);
u32 cpu_get_timebase_freq(void);

#endif
