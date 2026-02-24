#ifndef AOSD_SCHED_H
#define AOSD_SCHED_H

#include <aosd/sched_types.h>
#include <aosd/spinlock.h>

#define DEFAULT_TIME_SLICE 5
#define NR_TASKS 128

void switch_context(struct context *prev, struct context *next);

void sched_init(void);
void sched_yield(void);
void sched_sleep(void *chan, spinlock_t *lock);
void sched_wake_up(void *chan);
void sched_exit(int status);
int sched_wait(int *status, pid_t *pid);
void scheduler(void);

struct task *task_create(void);
void task_destroy(struct task *task);

void fork_return(void);

struct task *current_task(void);

void task_set_killed(struct task *t);
bool task_is_killed(struct task *t);

void show_all_tasks(void);

extern struct task *init_task;
extern struct task tasks[NR_TASKS];

#endif
