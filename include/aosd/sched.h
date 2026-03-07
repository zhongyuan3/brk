#ifndef AOSD_SCHED_H
#define AOSD_SCHED_H

#include <aosd/lock.h>
#include <aosd/sched_types.h>
#include <uapi/aosd/resource.h>

#define DEFAULT_TIME_SLICE 5
#define NR_TASKS 128

void switch_context(struct context *prev, struct context *next);

void sched_init(void);
void sched_yield(void);
void sched_sleep(void *chan, spinlock_t *lock);
void sched_wake_up(void *chan);
void sched_exit(int status);
pid_t do_wait4(pid_t child_pid, int *status, int options, struct rusage *rus);
void scheduler(void);

struct task *task_alloc(void);
void task_free(struct task *task);

void fork_return(void);

struct task *current_task(void);

void task_set_killed(struct task *t);
bool task_is_killed(struct task *t);
int task_fork(void);
int task_set_brk(uint64_t addr);
int task_alloc_fd(struct task *t, struct file *f);

void task_dump(void);

uint64_t make_user_stack(void);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

void init_task_entry(void);

extern struct task *init_task;
extern struct task tasks[NR_TASKS];

#endif
