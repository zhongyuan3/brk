#ifndef _BRK_THREAD_H
#define _BRK_THREAD_H

#include <arch/asm/page.h>
#include <brk/asm/cpu.h>
#include <brk/cpu.h>
#include <brk/spinlock_types.h>
#include <brk/thread_types.h>
#include <brk/types.h>

#define KSTACK_PAGE_ORDER 2
#define KSTACK_SIZE (PAGE_SIZE * (1 << KSTACK_PAGE_ORDER))
#define USTACK_PAGE_ORDER 8
#define USTACK_SIZE (PAGE_SIZE * (1 << USTACK_PAGE_ORDER))

#define TIME_SLICE_MAX 5

struct process;

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
void thread_sleep(void *chan, spinlock_t *lock);
/* CPU time accounting, in nanoseconds. */
void thread_account_user(struct thread *thread, u64 ns);
void thread_account_system(struct thread *thread, u64 ns);
u64 thread_live_system_ns(struct thread *thread);
u64 thread_cpu_time_ns(struct thread *thread);
/* Called from the assembly return path immediately before sret to user. */
void trap_return_to_user(void);
bool thread_sleep_interruptible(void *chan, spinlock_t *lock);
bool thread_abort_pending(struct thread *thread);
void thread_wake_all(void *chan);
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
