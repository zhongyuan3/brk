#ifndef BRK_TASK_TYPES_H
#define BRK_TASK_TYPES_H

#include <arch/context.h>
#include <arch/trapframe.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <uapi/times.h>
#include <uapi/types.h>

#define TASK_NAME_MAX 32
#define NGROUPS_MAX 32

struct sigaction_table;
struct uvm_space;
struct file_desc_table;
struct file_system_info;

enum task_state {
	TASK_STATE_NEW,
	TASK_STATE_RUNNING,
	TASK_STATE_SLEEPING,
	TASK_STATE_ZOMBIE,
};

struct task_resource_usage {
	struct tms task_times;
	spinlock_t lock;
};

struct task {
	/* protected by tasks_lock */
	struct list_head list;

	/* protected by run_queue_lock/sleep_queue_lock */
	struct list_head queue;

	/* protected by wait_lock */
	struct task *parent;
	/* protected by wait_lock */
	struct list_head children;
	/* protected by wait_lock */
	struct list_head child;

	union {
		struct { /* leader thread */
			struct list_head siblings;
			unsigned int nr_threads;
		};
		struct { /* non-leader thread */
			struct list_head sibling;
			struct task *leader;
		};
	};

	spinlock_t lock;
	/* protected by @lock */
	void *chan;
	/* protected by @lock */
	unsigned long kernel_sp;
	/* protected by @lock */
	enum task_state state;
	/* protected by @lock */
	int exit_status;

	/*
	 * no lock protection
	 *
	 * read-only after initialization
	 */
	pid_t tgid;
	/*
	 * no lock protection
	 *
	 * read-only after initialization
	 */
	pid_t pid;
	/* Process group and session, changed only by the task itself. */
	pid_t pgid;
	pid_t sid;

	/* Process credentials, inherited on fork. */
	kuid_t uid;
	kuid_t euid;
	kuid_t suid;
	kuid_t fsuid;
	kgid_t gid;
	kgid_t egid;
	kgid_t sgid;
	kgid_t fsgid;
	kgid_t groups[NGROUPS_MAX];
	int ngroups;
	umode_t umask;
	u64 cap_effective;
	u64 cap_permitted;
	u64 cap_inheritable;

	struct sigaction_table *sigactions;
	/* protected by @sigactions->lock */
	u64 pending;
	/* protected by @sigactions->lock */
	u64 blocked;
	/* protected by @sigactions->lock */
	u64 sigframe_sp;
	/* protected by @sigactions->lock */
	bool in_handler;

	struct uvm_space *mm;

	struct file_desc_table *fdtable;

	struct file_system_info *fsinfo;

	/* only the leader thread of the thread group can free this */
	struct task_resource_usage *rsrc_usage;

	/* the following fields are private, not protected by any locks */
	struct riscv_trap_frame *frame; /* no lock protection */
	u64 kstack_base; /* no lock protection */
	u64 kstack_top; /* no lock protection */
	int oncpu; /* CPU currently running this task, -1 if not running */
	int cpu; /* CPU affinity; the task is only dispatched on this CPU */
	u64 ktime; /* no lock protection */
	u64 utime; /* no lock protection */
	int time_slice; /* no lock protection */
	char name[TASK_NAME_MAX]; /* no lock protection */
	bool newly_forked; /* no lock protection */
	void (*kthrd_fn)(void *); /* no lock protection */
	void *kthrd_data; /* no lock protection */
};

/*
 * Read-only snapshot of the parts of a process that are safe to look at
 * from outside the scheduler / tasks_lock.
 */
struct task_info {
	pid_t pid; /* thread-group id (tgid) */
	pid_t ppid; /* parent thread-group id */
	enum task_state state;
	int exit_status;
	bool killed;
	u64 utime;
	u64 ktime;
	u64 brk;
	char name[TASK_NAME_MAX];
};

#endif
