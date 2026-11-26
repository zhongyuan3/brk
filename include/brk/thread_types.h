#ifndef _BRK_THREAD_TYPES_H
#define _BRK_THREAD_TYPES_H

#include <arch/context.h>
#include <brk/list.h>
#include <brk/spinlock_types.h>
#include <brk/timer_list.h>
#include <brk/types.h>
#include <brk/wait_types.h>
#include <stdbool.h>
#include <uapi/types.h>

struct process;
struct trap_frame;
struct futex_bucket;
struct uvm_space;

struct futex_waiter {
	struct list_head node;
	struct futex_bucket *bucket;
	struct uvm_space *mm;
	struct thread *thread;
	u32 *uaddr;
	u32 bitset;
	bool queued;
	bool woken;
};

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
	/* User address cleared and futex-woken on exit. */
	int *clear_tid;

	/* Embedded waiter, so a thread can never leave a stale stack pointer. */
	struct futex_waiter futex;

	/* protected by run_queue_lock */
	struct list_head run_node;
	/* protected by wait_queue_lock; linked into wait_wq->waiters */
	struct list_head sleep_node;

	spinlock_t lock;
	/* protected by @lock */
	wait_queue_head_t *wait_wq;
	/* protected by @lock */
	virt_addr_t kernel_sp;
	/* protected by @lock */
	enum thread_state state;
	/* protected by @lock */
	int exit_status;

	/* protected by process->sigactions->lock */
	u64 pending;
	/* protected by process->sigactions->lock */
	u64 blocked;
	/* protected by process->sigactions->lock */
	user_addr_t sigframe_sp;
	/* protected by process->sigactions->lock */
	bool in_handler;

	/* the following fields are private, not protected by any locks */
	struct trap_frame *frame; /* no lock protection */
	virt_addr_t kstack_base; /* no lock protection */
	virt_addr_t kstack_top; /* no lock protection */
	int oncpu; /* CPU currently running this thread, -1 if not running */
	int cpu; /* CPU affinity; the thread is only dispatched on this CPU */
	/* CPU time accounting; only the current thread updates these. */
	u64 utime_ns; /* cumulative user time */
	u64 stime_ns; /* cumulative system time */
	u64 last_user_ns; /* timestamp of the last user-mode entry */
	u64 last_kernel_ns; /* timestamp of the last kernel-mode entry */
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
	/* Set while the thread runs its own exit path. */
	bool in_exit; /* no lock protection */
	void (*kthrd_fn)(void *); /* no lock protection */
	void *kthrd_data; /* no lock protection */
};

#endif
