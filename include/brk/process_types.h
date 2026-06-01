#ifndef BRK_PROCESS_TYPES_H
#define BRK_PROCESS_TYPES_H

#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <uapi/times.h>

#define PROCESS_NAME_MAX 32

struct trap_frame;
struct sigaction_table;

struct switch_frame {
	u64 ra;
	u64 sp;
	u64 s0;
	u64 s1;
	u64 s2;
	u64 s3;
	u64 s4;
	u64 s5;
	u64 s6;
	u64 s7;
	u64 s8;
	u64 s9;
	u64 s10;
	u64 s11;
};

enum process_state {
	PROCESS_STATE_NEW,
	PROCESS_STATE_RUNNING,
	PROCESS_STATE_SLEEPING,
	PROCESS_STATE_ZOMBIE,
};

struct process {
	struct list_head list;

	struct list_head queue;

	struct process *parent;
	struct list_head children;
	struct list_head child;

	spinlock_t lock;
	void *chan;
	struct switch_frame ctx;
	pid_t pid;
	enum process_state state;
	int exit_status;
	u64 pending;
	u64 blocked;
	struct sigaction_table *sigactions;
	bool in_handler;
	u64 sigframe_sp;

	struct uvm_space *mm;
	struct file_desc_table *fdtable;
	struct file_system_info *fsinfo;
	struct tms ptms;
	struct trap_frame *tf; /* points to the trap frame on kernel stack */
	u64 kstack_base;
	u64 kstack_top;
	u64 ktime;
	u64 utime;
	int time_slice;
	char name[PROCESS_NAME_MAX];
	bool irq_enabled;
};

struct cpu {
	struct process *handoff;
	struct switch_frame ctx;
	int irq_nest;
	bool irq_enabled;
};

/*
 * Read-only snapshot of the parts of a process that are safe to look at
 * from outside the scheduler / proc_lock.
 */
struct process_info {
	pid_t pid;
	pid_t ppid;
	enum process_state state;
	int exit_status;
	bool killed;
	u64 utime;
	u64 ktime;
	u64 brk;
	char name[PROCESS_NAME_MAX];
};

#endif