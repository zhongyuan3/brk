#ifndef AOSD_SCHED_H
#define AOSD_SCHED_H

#include <aosd/sched_types.h>

void switch_context(struct context *prev, struct context *next);
void switch_to(struct context *next);

void sched_init(void);
void sched_join(struct task *task);
void sched_yield(void);
void sched_sleep(void *chan);
void sched_wake_up(void *chan);
void sched_exit(int status);
pid_t sched_wait(int *status);
void start_scheduling(void);

struct task *task_create(void);
void task_destroy(struct task *task);

extern struct task *init_task;

#endif
