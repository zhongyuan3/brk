#ifndef AOSD_TIMER_H
#define AOSD_TIMER_H

#include <aosd/types.h>
#include <aosd/time.h>

void timer_init(void);
uint64_t timer_get_time(void);
void timer_set_next(void);
void timer_handle_int(void);
void timer_srand(void);
uint32_t timer_rand(void);

void walltime_get(struct timeval *tv);
void walltime_set(const struct timeval *tv);

uint64_t do_nanosleep(const struct timeval *dur, struct timeval *rem);

uint64_t jiffies_get(void);

#endif
