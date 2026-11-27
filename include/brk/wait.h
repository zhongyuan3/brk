#ifndef _BRK_WAIT_H
#define _BRK_WAIT_H

#include <brk/ktime.h>
#include <brk/spinlock.h>
#include <brk/thread.h>
#include <brk/wait_types.h>
#include <uapi/errno.h>

void wait_queue_init(wait_queue_head_t *wq, spinlock_t *lock, const char *name);

/*
 * Wake one/all waiters on @wq.  The caller must have updated the condition
 * before calling these; this is the same ordering rule as thread_wake_one()/
 * thread_wake_all().  It is safe to call them with or without @wq->lock held.
 */
int wake_up(wait_queue_head_t *wq);
int wake_up_all(wait_queue_head_t *wq);

/*
 * Wait until @cond becomes true.  @cond is evaluated with the wait queue's
 * lock held, so it must be side-effect free and protected by that lock.
 */
#define wait_event(wq, cond) wait_event_locked((wq), (wq)->lock, (cond))

#define wait_event_locked(wq, lock, cond)                                \
	do {                                                             \
		for (;;) {                                               \
			irqstate_t __flags = spin_lock_irqsave((lock));  \
			if (cond) {                                      \
				spin_unlock_irqrestore((lock), __flags); \
				break;                                   \
			}                                                \
			thread_sleep_on((wq), (lock));                   \
			spin_unlock_irqrestore((lock), __flags);         \
		}                                                        \
	} while (0)

/*
 * Interruptible variant.  Returns 0 when @cond became true, or -EINTR when a
 * signal/group exit aborted the wait.
 */
#define wait_event_interruptible(wq, cond) \
	wait_event_interruptible_locked((wq), (wq)->lock, (cond))

#define wait_event_interruptible_locked(wq, lock, cond)                      \
	({                                                                   \
		int __ret = 0;                                               \
		for (;;) {                                                   \
			irqstate_t __flags = spin_lock_irqsave((lock));      \
			if (cond) {                                          \
				spin_unlock_irqrestore((lock), __flags);     \
				break;                                       \
			}                                                    \
			__ret = thread_sleep_on_interruptible((wq), (lock)); \
			spin_unlock_irqrestore((lock), __flags);             \
			if (__ret < 0)                                       \
				break;                                       \
		}                                                            \
		__ret;                                                       \
	})

/*
 * Timed waits.  @timeout_ns is relative and uses the monotonic clock.  The
 * definitions return 0 when the condition became true, -ETIMEDOUT on expiry,
 * and (for the interruptible variants) -EINTR when a signal aborted the wait.
 *
 * These use the current thread's single sleep timer, so a timed wait must not
 * be nested inside another timed wait.
 */
#define wait_event_timeout(wq, cond, timeout_ns) \
	wait_event_timeout_locked((wq), (wq)->lock, (cond), (timeout_ns))

#define wait_event_timeout_locked(wq, lock, cond, timeout_ns)              \
	({                                                                 \
		int __ret = 0;                                             \
		ktime_t __timeout = (timeout_ns);                          \
		u64 __deadline = 0;                                        \
		if (__timeout > 0)                                         \
			__deadline = ktime_get_mono_ns() + (u64)__timeout; \
		for (;;) {                                                 \
			irqstate_t __flags = spin_lock_irqsave((lock));    \
			if (cond) {                                        \
				spin_unlock_irqrestore((lock), __flags);   \
				break;                                     \
			}                                                  \
			if (__timeout <= 0) {                              \
				spin_unlock_irqrestore((lock), __flags);   \
				__ret = -ETIMEDOUT;                        \
				break;                                     \
			}                                                  \
			u64 __now = ktime_get_mono_ns();                   \
			if ((s64)(__now - __deadline) >= 0) {              \
				spin_unlock_irqrestore((lock), __flags);   \
				__ret = -ETIMEDOUT;                        \
				break;                                     \
			}                                                  \
			thread_arm_sleep_timer((lock), __deadline);        \
			thread_sleep_on((wq), (lock));                     \
			spin_unlock_irqrestore((lock), __flags);           \
			thread_disarm_sleep_timer();                       \
		}                                                          \
		__ret;                                                     \
	})

#define wait_event_interruptible_timeout(wq, cond, timeout_ns)            \
	wait_event_interruptible_timeout_locked((wq), (wq)->lock, (cond), \
						(timeout_ns))

#define wait_event_interruptible_timeout_locked(wq, lock, cond, timeout_ns)  \
	({                                                                   \
		int __ret = 0;                                               \
		ktime_t __timeout = (timeout_ns);                            \
		u64 __deadline = 0;                                          \
		if (__timeout > 0)                                           \
			__deadline = ktime_get_mono_ns() + (u64)__timeout;   \
		for (;;) {                                                   \
			irqstate_t __flags = spin_lock_irqsave((lock));      \
			if (cond) {                                          \
				spin_unlock_irqrestore((lock), __flags);     \
				break;                                       \
			}                                                    \
			if (__timeout <= 0) {                                \
				spin_unlock_irqrestore((lock), __flags);     \
				__ret = -ETIMEDOUT;                          \
				break;                                       \
			}                                                    \
			u64 __now = ktime_get_mono_ns();                     \
			if ((s64)(__now - __deadline) >= 0) {                \
				spin_unlock_irqrestore((lock), __flags);     \
				__ret = -ETIMEDOUT;                          \
				break;                                       \
			}                                                    \
			thread_arm_sleep_timer((lock), __deadline);          \
			__ret = thread_sleep_on_interruptible((wq), (lock)); \
			spin_unlock_irqrestore((lock), __flags);             \
			thread_disarm_sleep_timer();                         \
			if (__ret < 0)                                       \
				break;                                       \
		}                                                            \
		__ret;                                                       \
	})

#endif
