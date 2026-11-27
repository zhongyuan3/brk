#include <brk/assert.h>
#include <brk/atomic.h>
#include <brk/completion.h>
#include <brk/spinlock.h>
#include <brk/thread.h>
#include <brk/wait.h>
#include <uapi/errno.h>

void completion_init(completion_t *comp, const char *name)
{
	comp->done = 0;
	spinlock_init(&comp->wait_lock, "completion.wait_lock");
	wait_queue_init(&comp->wait, &comp->wait_lock, name);
	comp->name = name;
}

void reinit_completion(completion_t *comp)
{
	atomic_store_n(&comp->done, 0, ATOMIC_RELEASE);
}

void wait_for_completion(completion_t *comp)
{
	irqstate_t flags;

	kassert(preempt_count() == 0);

	flags = spin_lock_irqsave(&comp->wait_lock);
	while (!comp->done)
		thread_sleep_on(&comp->wait, &comp->wait_lock);
	spin_unlock_irqrestore(&comp->wait_lock, flags);
}

int wait_for_completion_interruptible(completion_t *comp)
{
	irqstate_t flags;
	int err = 0;

	kassert(preempt_count() == 0);

	flags = spin_lock_irqsave(&comp->wait_lock);
	while (!comp->done) {
		err = thread_sleep_on_interruptible(&comp->wait,
						    &comp->wait_lock);
		if (err < 0)
			break;
	}
	spin_unlock_irqrestore(&comp->wait_lock, flags);
	return err;
}

void complete(completion_t *comp)
{
	irqstate_t flags;
	bool wake;

	/*
	 * Wake while holding wait_lock.  A completion may live on the waiter's
	 * stack; keeping the lock until wake_all() returns prevents the waiter
	 * from observing @done and destroying the completion underneath us.
	 */
	flags = spin_lock_irqsave(&comp->wait_lock);
	wake = !comp->done;
	comp->done = 1;
	if (wake)
		thread_wake_all(&comp->wait);
	spin_unlock_irqrestore(&comp->wait_lock, flags);
}

void complete_all(completion_t *comp)
{
	irqstate_t flags;

	flags = spin_lock_irqsave(&comp->wait_lock);
	comp->done = 1;
	thread_wake_all(&comp->wait);
	spin_unlock_irqrestore(&comp->wait_lock, flags);
}

bool completion_done(const completion_t *comp)
{
	return atomic_load_n(&comp->done, ATOMIC_ACQUIRE) != 0;
}
