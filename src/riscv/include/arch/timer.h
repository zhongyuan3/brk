#ifndef ARCH_TIMER_H
#define ARCH_TIMER_H

void timer_init_hart(void);
void program_next_tick(void);
void handle_tick(void);

#endif
