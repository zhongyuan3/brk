#ifndef BRK_CPU_H
#define BRK_CPU_H

#include <brk/types.h>

void cpu_set_timebase_freq(uint32_t freq);
uint32_t cpu_get_timebase_freq(void);

#endif
