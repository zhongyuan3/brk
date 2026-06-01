#ifndef BRK_TASK_H
#define BRK_TASK_H

#include <brk/asm.h>
#include <brk/fdtable.h>
#include <brk/fs_types.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/path.h>
#include <brk/signal_types.h>
#include <brk/spinlock_types.h>
#include <brk/task_types.h>
#include <brk/types.h>
#include <uapi/brk/limits.h>
#include <uapi/resource.h>
#include <uapi/signal.h>

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 4
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))

#define TIME_SLICE_MAX 5

void task_init_user(void);

void task_cache_init(void);
struct task_control_block *task_create(void);
void task_destroy(struct task_control_block *task);
void task_set_killed(struct task_control_block *task);
bool task_is_killed(struct task_control_block *task);
void task_exit(int status);
void task_exit_normal(int code);
void task_exit_signal(int sig);
int task_fork(void);
int task_set_brk(u64 addr);
void task_dump(void);

void task_yield(void);
void task_sleep(void *chan, spinlock_t *lock);
void task_wake_all(void *chan);
pid_t task_wait(pid_t cpid, int *status, int options, struct rusage *rus);
void task_wake_spec(struct task_control_block *task);
void task_sched(void);
void task_sched_resume(void);
void task_scheduler(void);
void task_join(struct task_control_block *task);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

/*
 * Copy every live pid into @out (up to @max entries). Returns the number
 * of pids written. The order is the global creation order maintained by
 * task_create().
 */
int task_snapshot_pids(pid_t *out, int max);

/*
 * Snapshot a single process's fields into @info. Returns true on
 * success, false if no such pid exists.
 */
bool task_get_info(pid_t pid, struct task_info *info);

/* Lightweight existence check. */
bool task_pid_exists(pid_t pid);

void push_off(void);
void pop_off(void);

struct task_control_block *current_task(void);
struct cpu *current_cpu(void);
cpuid_t current_cpuid(void);
void set_current_task(struct task_control_block *task);
void set_current_cpuid(cpuid_t cpuid);

void switch_context(struct switch_frame *prev, struct switch_frame *next);

extern cpuid_t boot_cpuid;
extern struct task_control_block *initial_task;
extern struct list_head tasks;
extern spinlock_t tasks_lock;
extern struct cpu cpus[NR_CPUS];
extern spinlock_t wait_lock;

#endif
