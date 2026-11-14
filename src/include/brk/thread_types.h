#ifndef BRK_THREAD_TYPES_H
#define BRK_THREAD_TYPES_H

#include <arch/context.h>
#include <arch/trapframe.h>
#include <brk/spinlock_types.h>
#include <brk/timer_list.h>
#include <brk/types.h>
#include <stdbool.h>
#include <uapi/types.h>

struct process;

enum thread_state {
	THREAD_STATE_NEW,
	THREAD_STATE_RUNNING,
	THREAD_STATE_SLEEPING,
	THREAD_STATE_ZOMBIE,
};

/*
 * A thread is the only schedulable entity.  All process-wide state lives in
 * struct process and is reached through @process.
 */
struct thread {
	/* protected by process_list_lock */
	struct list_head list;

	/* process->threads, protected by process->lock */
	struct list_head proc_threads;

	/* Immutable after creation. */
	struct process *process;
	/* Immutable after creation; leader tid == process->pid. */
	pid_t tid;

	/* protected by run_queue_lock/sleep_queue_lock */
	struct list_head queue;

	spinlock_t lock;
	/* protected by @lock */
	void *chan;
	/* protected by @lock */
	unsigned long kernel_sp;
	/* protected by @lock */
	enum thread_state state;
	/* protected by @lock */
	int exit_status;

	/* protected by process->sigactions->lock */
	u64 pending;
	/* protected by process->sigactions->lock */
	u64 blocked;
	/* protected by process->sigactions->lock */
	u64 sigframe_sp;
	/* protected by process->sigactions->lock */
	bool in_handler;

	/* the following fields are private, not protected by any locks */
	struct trap_frame *frame; /* no lock protection */
	u64 kstack_base; /* no lock protection */
	u64 kstack_top; /* no lock protection */
	int oncpu; /* CPU currently running this thread, -1 if not running */
	int cpu; /* CPU affinity; the thread is only dispatched on this CPU */
	u64 ktime; /* no lock protection */
	u64 utime; /* no lock protection */
	int time_slice; /* no lock protection */

	/*
	 * Sleep timeout owned by the thread, so a fatal signal that exits the
	 * thread from the trap path cannot leave a timer pointing at a freed
	 * kernel stack.  process_destroy() cancels it before freeing the
	 * thread.
	 */
	struct timer_list sleep_timer; /* no lock protection */
	spinlock_t *sleep_lock; /* published before timer_add() */

	bool newly_forked; /* no lock protection */
	bool is_init_thread; /* static boot/idle thread, never destroyed */
	void (*kthrd_fn)(void *); /* no lock protection */
	void *kthrd_data; /* no lock protection */
};

#endif
