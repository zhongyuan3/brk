#ifndef AOSD_TIMER_H
#define AOSD_TIMER_H

#include <aosd/types.h>

void timer_init(void);
uint64_t timer_get_time(void);
void timer_set_next(void);
void timer_handle_int(void);

#endif
