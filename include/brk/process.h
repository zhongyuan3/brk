#ifndef BRK_PROCESS_H
#define BRK_PROCESS_H

#include <brk/asm.h>
#include <brk/fdtable.h>
#include <brk/fs_types.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/path.h>
#include <brk/process_types.h>
#include <brk/signal_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <uapi/brk/limits.h>
#include <uapi/resource.h>
#include <uapi/signal.h>

#define KSTACK_PAGE_ORDER 1
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 4
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))

#define TIME_SLICE_MAX 5

void proc_init_user(void);

void proc_cache_init(void);
struct process *proc_alloc(void);
void proc_free(struct process *proc);
void proc_set_killed(struct process *proc);
bool proc_is_killed(struct process *proc);
void proc_exit(int status);
void proc_exit_normal(int code);
void proc_exit_signal(int sig);
int proc_fork(void);
int proc_set_brk(u64 addr);
void proc_dump(void);

void proc_yield(void);
void proc_sleep(void *chan, spinlock_t *lock);
void proc_wake_up(void *chan);
void proc_wake_all(void *chan);
pid_t proc_wait(pid_t cpid, int *status, int options, struct rusage *rus);
void proc_wake_process(struct process *proc);
void proc_sched(void);
void proc_sched_resume(void);
void proc_scheduler(void);
void proc_join(struct process *proc);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

/*
 * Copy every live pid into @out (up to @max entries). Returns the number
 * of pids written. The order is the global creation order maintained by
 * proc_alloc().
 */
int proc_snapshot_pids(pid_t *out, int max);

/*
 * Snapshot a single process's fields into @info. Returns true on
 * success, false if no such pid exists.
 */
bool proc_get_info(pid_t pid, struct process_info *info);

/* Lightweight existence check. */
bool proc_pid_exists(pid_t pid);

void push_off(void);
void pop_off(void);

struct process *current_process(void);
struct cpu *current_cpu(void);
cpuid_t current_cpuid(void);
void set_current_process(struct process *proc);
void set_current_cpuid(cpuid_t cpuid);

void switch_context(struct switch_frame *prev, struct switch_frame *next);

extern cpuid_t init_cpuid;
extern struct process *init_proc;
extern struct list_head procs;
extern spinlock_t procs_lock;
extern struct cpu cpus[NR_CPUS];
extern spinlock_t wait_lock;

#endif
