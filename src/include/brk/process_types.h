#ifndef BRK_PROCESS_TYPES_H
#define BRK_PROCESS_TYPES_H

#include <brk/spinlock_types.h>
#include <brk/thread_types.h>
#include <brk/types.h>
#include <stdbool.h>
#include <uapi/times.h>
#include <uapi/types.h>

#define PROCESS_NAME_MAX 32
#define NGROUPS_MAX 32

struct sigaction_table;
struct uvm_space;
struct file_desc_table;
struct file_system_info;

/* Process credentials, inherited on fork and shared by all threads. */
struct cred {
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
};

struct process_resource_usage {
	struct tms times;
	spinlock_t lock;
};

/* process->flags */
#define PROCESS_FLAG_KTHREAD (1u << 0)
#define PROCESS_FLAG_STATIC (1u << 1) /* boot/idle, never freed by wait */
#define PROCESS_FLAG_IDLE (1u << 2)

/*
 * A process is a thread group and the owner of all process-wide resources.
 * It is never placed on a run queue; only its threads are schedulable.
 */
struct process {
	/* protected by process_list_lock */
	struct list_head list;

	/* protected by wait_lock */
	struct process *parent;
	/* protected by wait_lock */
	struct list_head children;
	/* protected by wait_lock */
	struct list_head sibling;

	/* process-wide mutable state */
	spinlock_t lock;
	/* protected by @lock */
	struct list_head threads;
	/* protected by @lock; includes zombie threads */
	unsigned int nr_threads;
	/* protected by @lock; threads that have not exited */
	unsigned int nr_live;
	/* protected by @lock */
	bool exited;
	/* protected by @lock; last thread has completely left the CPU */
	bool reap_ready;
	/* protected by @lock */
	int exit_status;
	/* protected by @lock */
	struct thread *last_thread;
	/* Immutable after creation; may point at a zombie. */
	struct thread *leader;

	/*
	 * Immutable after creation.
	 *
	 * Process group and session are changed only for the whole process.
	 */
	pid_t pid;
	pid_t pgid;
	pid_t sid;

	/* Process-owned resources. */
	struct uvm_space *mm;
	struct file_desc_table *fdtable;
	struct file_system_info *fsinfo;
	struct sigaction_table *sigactions;
	struct process_resource_usage *rsrc_usage;

	struct cred cred;

	/* protected by @sigactions->lock */
	u64 shared_pending;

	char name[PROCESS_NAME_MAX]; /* no lock protection */
	unsigned int flags; /* no lock protection after creation */
};

/*
 * Read-only snapshot of the parts of a process that are safe to look at
 * from outside the scheduler / process_list_lock.
 */
struct process_info {
	pid_t pid;
	pid_t ppid;
	enum thread_state state;
	int exit_status;
	bool killed;
	u64 utime;
	u64 ktime;
	u64 brk;
	unsigned int nr_threads;
	char name[PROCESS_NAME_MAX];
};

#endif
