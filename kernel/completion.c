#include <brk/assert.h>
#include <brk/completion.h>
#include <brk/spinlock.h>
#include <brk/thread.h>
#include <uapi/errno.h>

void completion_init(completion_t *comp, const char *name)
{
	comp->done = 0;
	spinlock_init(&comp->wait_lock, "completion.wait_lock");
	comp->name = name;
}

void reinit_completion(completion_t *comp)
{
	__atomic_store_n(&comp->done, 0, __ATOMIC_RELEASE);
}

void wait_for_completion(completion_t *comp)
{
	irqstate_t flags;

	kassert(preempt_count() == 0);

	flags = spin_lock_irqsave(&comp->wait_lock);
	while (!comp->done)
		thread_sleep_on(comp, &comp->wait_lock);
	spin_unlock_irqrestore(&comp->wait_lock, flags);
}

int wait_for_completion_interruptible(completion_t *comp)
{
	irqstate_t flags;
	int err = 0;

	kassert(preempt_count() == 0);

	flags = spin_lock_irqsave(&comp->wait_lock);
	while (!comp->done) {
		err = thread_sleep_on_interruptible(comp, &comp->wait_lock);
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

	flags = spin_lock_irqsave(&comp->wait_lock);
	wake = !comp->done;
	comp->done = 1;
	spin_unlock_irqrestore(&comp->wait_lock, flags);

	if (wake)
		thread_wake_all(comp);
}

void complete_all(completion_t *comp)
{
	irqstate_t flags;

	flags = spin_lock_irqsave(&comp->wait_lock);
	comp->done = 1;
	spin_unlock_irqrestore(&comp->wait_lock, flags);

	thread_wake_all(comp);
}

bool completion_done(const completion_t *comp)
{
	return __atomic_load_n(&comp->done, __ATOMIC_ACQUIRE) != 0;
}
