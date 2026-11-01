#ifndef BRK_CLOCKSOURCE_H
#define BRK_CLOCKSOURCE_H

#include <brk/types.h>

int clocksource_init(void);
void clocksource_set_timebase_freq(uint32_t freq);
uint32_t clocksource_get_timebase_freq(void);

#endif