#ifndef BRK_PROCESS_H
#define BRK_PROCESS_H

#include <brk/cpu.h>
#include <brk/fdtable.h>
#include <brk/fs_types.h>
#include <brk/mm_types.h>
#include <brk/process_types.h>
#include <brk/signal_types.h>
#include <brk/spinlock_types.h>
#include <brk/thread_types.h>
#include <brk/types.h>
#include <uapi/resource.h>
#include <uapi/times.h>
#include <uapi/types.h>

struct process_create_args {
	struct process *parent;
	pid_t pgid;
	pid_t sid;
	u64 blocked;
	const char *name;
	unsigned int flags;
	void (*fn)(void);
};

void process_init_main(void);
void process_cache_init(void);

struct process *process_create(struct process_create_args *args);
struct process *process_create_idle(cpuid_t cpuid);
void process_destroy(struct process *process);

int process_fork(void);
void process_exit_group(int status);
pid_t process_wait(pid_t pid, int *status, int options, struct rusage *rus,
		   kuid_t *uid);

pid_t process_get_pgid(pid_t pid);
pid_t process_get_sid(pid_t pid);
bool process_pgrp_exists(pid_t pgid, pid_t sid);
bool process_pid_exists(pid_t pid);
bool process_get_info(pid_t pid, struct process_info *info);
int process_snapshot_pids(pid_t *out, int max);

kuid_t process_get_uid(struct process *process);
kuid_t process_get_euid(struct process *process);
kuid_t process_get_fsuid(struct process *process);
kgid_t process_get_fsgid(struct process *process);
void process_get_fsids(struct process *process, kuid_t *fsuid, kgid_t *fsgid);
umode_t process_get_umask(struct process *process);
bool process_has_cap(struct process *process, int cap);

int process_get_times(struct process *process, struct tms *times);
void process_add_system_time(struct process *process, u64 time);
void process_add_user_time(struct process *process, u64 time);
void process_add_child_system_time(struct process *process, u64 time);
void process_add_child_user_time(struct process *process, u64 time);
u64 process_get_system_time(struct process *process);
u64 process_get_user_time(struct process *process);
u64 process_get_child_system_time(struct process *process);
u64 process_get_child_user_time(struct process *process);

pid_t pid_alloc(void);
void pid_free(pid_t pid);

static inline bool process_is_kthread(const struct process *process)
{
	return process->flags & PROCESS_FLAG_KTHREAD;
}

static inline struct process *current_process(void)
{
	return current_thread()->process;
}

static inline bool process_group_exiting(struct process *process)
{
	return __atomic_load_n(&process->group_exiting, __ATOMIC_ACQUIRE);
}

static inline int process_group_exit_status(struct process *process)
{
	return __atomic_load_n(&process->group_exit_status, __ATOMIC_RELAXED);
}

struct process_fsids {
	kuid_t fsuid;
	kgid_t fsgid;
};

static inline struct process_fsids process_current_fsids(void)
{
	struct process_fsids ids;

	process_get_fsids(current_process(), &ids.fsuid, &ids.fsgid);
	return ids;
}

extern struct process *init_reaper;
extern struct list_head processes;
extern spinlock_t process_list_lock;
extern spinlock_t wait_lock;

#endif
