#ifndef BRK_TASK_TYPES_H
#define BRK_TASK_TYPES_H

#include <arch/processor.h>
#include <arch/trapframe.h>
#include <brk/lib/types.h>
#include <brk/lock/spinlock_types.h>
#include <uapi/times.h>
#include <uapi/types.h>

#define TASK_NAME_MAX 32

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

struct task_control_block {
	/* protected by tasks_lock */
	struct list_head list;

	/* protected by run_queue_lock/sleep_queue_lock */
	struct list_head queue;

	/* protected by wait_lock */
	struct task_control_block *parent;
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
			struct task_control_block *leader;
		};
	};

	spinlock_t lock;
	/* protected by @lock */
	void *chan;
	/* protected by @lock */
	struct switch_frame ctx;
	/* protected by @lock */
	enum task_state state;
	/* protected by @lock */
	int exit_status;

	/* never modified after creation, not protected by any locks */
	pid_t tgid;
	/* never modified after creation, not protected by any locks */
	pid_t pid;

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
	struct trap_frame *tf; /* points to the trap frame on kernel stack */
	u64 kstack_base;
	u64 kstack_top;
	u64 ktime; /* last time the task was scheduled or entered kernel mode */
	u64 utime; /* last time the task entered user mode */
	int time_slice;
	char name[TASK_NAME_MAX];
	bool irq_enabled;
};

struct cpu {
	struct task_control_block *handoff;
	struct switch_frame ctx;
	int irq_nest;
	bool irq_enabled;
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
