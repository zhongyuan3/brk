#ifndef AOSD_CPU_H
#define AOSD_CPU_H

#include <aosd/types.h>

void cpu_set_timebase_freq(uint32_t freq);
uint32_t cpu_get_timebase_freq(void);

#endif
