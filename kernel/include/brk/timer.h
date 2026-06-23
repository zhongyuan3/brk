#ifndef BRK_TIMER_H
#define BRK_TIMER_H

#include <brk/types.h>

void timer_init(void);
uint64_t timer_get_time(void);
void timer_set_next(void);
void timer_handle_int(void);
void timer_srand(void);
uint32_t timer_rand(void);

#endif
