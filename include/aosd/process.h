#ifndef AOSD_PROCESS_H
#define AOSD_PROCESS_H

#include <aosd/lock.h>
#include <aosd/process_types.h>
#include <aosd/types.h>
#include <uapi/aosd/resource.h>

#define PROCESS_TIME_SLICE 5
#define NR_PROCESSES 128

void proc_init(void);
void proc_yield(void);
void proc_sleep(void *chan, spinlock_t *lock);
void proc_wake_up(void *chan);
void proc_exit(int status);
pid_t proc_wait(pid_t child_pid, int *status, int options, struct rusage *rus);
void proc_scheduler(void);
struct process *proc_alloc(void);
void proc_free(struct process *proc);
void proc_fork_return(void);
struct process *proc_get_current(void);
void proc_set_killed(struct process *proc);
bool proc_is_killed(struct process *proc);
int proc_fork(void);
int proc_set_brk(uint64_t addr);
int proc_alloc_fd(struct process *proc, struct file *fp);
void proc_dump(void);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

#endif
