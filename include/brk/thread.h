#ifndef _BRK_THREAD_H
#define _BRK_THREAD_H

#include <arch/asm/page.h>
#include <brk/asm/cpu.h>
#include <brk/cpu.h>
#include <brk/process_types.h>
#include <brk/spinlock_types.h>
#include <brk/thread_types.h>
#include <brk/types.h>
#include <brk/wait_types.h>

#define KSTACK_PAGE_ORDER 2
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 8
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))

#define TIME_SLICE_MAX 5

void thread_cache_init(void);
struct thread *thread_create(struct process *process, void (*fn)(void *),
			     void *data);
struct thread *thread_create_user(struct process *process, user_addr_t stack,
				  user_addr_t entry, user_addr_t arg,
				  user_addr_t tls, int *clear_tid);
void thread_start(struct thread *thread);

void thread_exit(int status);
void thread_exit_normal(int code);
void thread_exit_signal(int sig);

void thread_yield(void);
/* Scheduler tick: run from the per-CPU timer interrupt. */
void sched_tick(void);

/*
 * Sleep on @wq until a matching thread_wake_*() or wake_up*() call.
 *
 * @wake_lock must be held by the caller with interrupts disabled.  It is
 * released before the context switch and reacquired before return.  It need
 * not be the lock that directly protects the condition; it is the lock that
 * serializes wait publication with the waker's condition update.  The caller
 * must recheck its condition in a loop after return.
 */
void thread_sleep_on(wait_queue_head_t *wq, spinlock_t *wake_lock);

/*
 * Like thread_sleep_on(), but abort the sleep when a signal is pending or the
 * process group is exiting.  Returns 0 after a normal sleep, or -EINTR when
 * the sleep was aborted; the caller must unwind instead of re-entering.
 */
int thread_sleep_on_interruptible(wait_queue_head_t *wq, spinlock_t *wake_lock);

/* CPU time accounting, in nanoseconds. */
void thread_account_user(struct thread *thread, u64 ns);
void thread_account_system(struct thread *thread, u64 ns);
u64 thread_live_system_ns(struct thread *thread);
u64 thread_cpu_time_ns(struct thread *thread);
/* Called from the assembly return path immediately before sret to user. */
void trap_return_to_user(void);
bool thread_abort_pending(struct thread *thread);
/* Wake one/all threads sleeping on @wq; returns the number woken. */
int thread_wake_one(wait_queue_head_t *wq);
int thread_wake_all(wait_queue_head_t *wq);
void thread_wake(struct thread *thread);

/*
 * Arm/disarm the current thread's one-shot sleep timeout.  The timer is owned
 * by struct thread, so it remains valid until process_destroy() cancels it.
 * The caller must disarm a previous timeout before arming a new one.
 */
void thread_arm_sleep_timer(spinlock_t *lock, u64 expires_ns);
void thread_disarm_sleep_timer(void);

void sched_enqueue(struct thread *thread);
void sched_resume(void);
void sched_start(void);

struct thread *kthread_create(void (*fn)(void *), void *data);

static inline bool thread_is_leader(struct thread *thread)
{
	return thread->tid == thread->process->pid;
}

#endif
