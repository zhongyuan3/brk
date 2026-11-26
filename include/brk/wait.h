#ifndef _BRK_WAIT_H
#define _BRK_WAIT_H

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

#endif
