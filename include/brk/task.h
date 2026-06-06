#ifndef BRK_TASK_H
#define BRK_TASK_H

#include <arch/smp.h>
#include <arch/task.h>
#include <brk/fdtable.h>
#include <brk/fs_types.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/path.h>
#include <brk/processor.h>
#include <brk/signal_types.h>
#include <brk/spinlock_types.h>
#include <brk/task_types.h>
#include <brk/types.h>
#include <uapi/brk/limits.h>
#include <uapi/resource.h>
#include <uapi/signal.h>
#include <uapi/times.h>
#include <uapi/types.h>

#define TIME_SLICE_MAX 5

/*
 * Optional fields are borrowed from the caller. task_create() and
 * signal_init() each acquire their own reference for the resources they
 * own (fdtable/fsinfo and sigactions respectively).
 */
struct task_create_args {
	pid_t tgid;
	struct uvm_space *mm;
	struct file_desc_table *fdtable;
	struct file_system_info *fsinfo;
	struct task_resource_usage *rsrc_usage;
	struct sigaction_table *sigactions;
	u64 blocked; /* if @sigactions is not NULL, @blocked must be valid */
	void (*fn)(void); /* function to run after the task is created */
};

void task_init_user(void);

void task_cache_init(void);
struct task_control_block *task_create(struct task_create_args *args);
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

int task_get_times(struct task_control_block *task, struct tms *times);
void task_add_system_time(struct task_control_block *task, u64 time);
void task_add_user_time(struct task_control_block *task, u64 time);
void task_add_child_system_time(struct task_control_block *task, u64 time);
void task_add_child_user_time(struct task_control_block *task, u64 time);
u64 task_get_system_time(struct task_control_block *task);
u64 task_get_user_time(struct task_control_block *task);
u64 task_get_child_system_time(struct task_control_block *task);
u64 task_get_child_user_time(struct task_control_block *task);

static inline bool task_is_leader(struct task_control_block *task)
{
	return task->pid == task->tgid;
}

struct task_resource_usage *task_rusage_alloc(void);
void task_rusage_free(struct task_resource_usage *rsrc_usage);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

/*
 * Copy every live thread-group id into @out (up to @max entries).
 * Each tgid appears once even when the group has multiple threads.
 */
int task_snapshot_pids(pid_t *out, int max);

/*
 * Snapshot a thread group into @info. @tgid is the thread-group id
 * (as returned by getpid), not a per-thread tid.
 */
bool task_get_info(pid_t tgid, struct task_info *info);

/* Return true if a thread group with @tgid exists. */
bool task_pid_exists(pid_t tgid);

extern cpuid_t boot_cpuid;
extern struct task_control_block *initial_task;
extern struct list_head tasks;
extern spinlock_t tasks_lock;
extern struct cpu cpus[NR_CPUS];
extern spinlock_t wait_lock;

#endif
