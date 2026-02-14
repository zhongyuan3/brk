#ifndef AOSD_SCHED_SCHED_H
#define AOSD_SCHED_SCHED_H

#include <aosd/sched/types.h>

void switch_context(struct context *prev, struct context *next);

void sched_init(void);
void sched_join(struct task *task);
void sched_yield(void);
void start_scheduler(void);

struct task *task_create(void);
void task_destroy(struct task *task);

#endif
