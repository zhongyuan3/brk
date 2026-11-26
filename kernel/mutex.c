#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/mutex.h>
#include <brk/panic.h>
#include <brk/process.h>
#include <brk/spinlock.h>
#include <brk/thread.h>
#include <uapi/errno.h>

struct mutex_waiter {
	struct list_head node;
	struct thread *thread;
	bool granted;
};

static bool mutex_try_acquire(mutex_t *mutex, struct thread *self)
{
	struct thread *expected = NULL;

	return __atomic_compare_exchange_n(&mutex->owner, &expected, self,
					   false, __ATOMIC_ACQUIRE,
					   __ATOMIC_RELAXED);
}

void mutex_init(mutex_t *mutex, const char *name)
{
	mutex->owner = NULL;
	spinlock_init(&mutex->wait_lock, "mutex.wait_lock");
	list_init(&mutex->wait_list);
	mutex->name = name;
}

void mutex_lock(mutex_t *mutex)
{
	struct thread *self = current_thread();
	struct mutex_waiter waiter;
	irqstate_t flags;

	if (mutex_try_acquire(mutex, self))
		return;

	kassert(preempt_count() == 0);

	flags = spin_lock_irqsave(&mutex->wait_lock);
	if (mutex->owner == self)
		panic("mutex_lock: recursive name=%s\n", mutex->name);
	if (!mutex->owner) {
		mutex->owner = self;
		spin_unlock_irqrestore(&mutex->wait_lock, flags);
		return;
	}

	waiter.thread = self;
	waiter.granted = false;
	list_add_tail(&waiter.node, &mutex->wait_list);

	while (!waiter.granted)
		thread_sleep_on(mutex, &mutex->wait_lock);

	spin_unlock_irqrestore(&mutex->wait_lock, flags);
}

int mutex_lock_interruptible(mutex_t *mutex)
{
	struct thread *self = current_thread();
	struct mutex_waiter waiter;
	irqstate_t flags;
	int err = 0;

	if (mutex_try_acquire(mutex, self))
		return 0;

	kassert(preempt_count() == 0);

	flags = spin_lock_irqsave(&mutex->wait_lock);
	if (mutex->owner == self)
		panic("mutex_lock_interruptible: recursive name=%s\n",
		      mutex->name);
	if (!mutex->owner) {
		mutex->owner = self;
		spin_unlock_irqrestore(&mutex->wait_lock, flags);
		return 0;
	}

	waiter.thread = self;
	waiter.granted = false;
	list_add_tail(&waiter.node, &mutex->wait_list);

	while (!waiter.granted) {
		err = thread_sleep_on_interruptible(mutex, &mutex->wait_lock);
		if (err < 0) {
			/*
			 * A handoff may have raced with the abort.  If we were
			 * granted ownership, keep it; otherwise leave the wait
			 * list before returning.
			 */
			if (waiter.granted) {
				err = 0;
				break;
			}
			list_del_init(&waiter.node);
			break;
		}
	}

	spin_unlock_irqrestore(&mutex->wait_lock, flags);
	return err;
}

bool mutex_trylock(mutex_t *mutex)
{
	return mutex_try_acquire(mutex, current_thread());
}

void mutex_unlock(mutex_t *mutex)
{
	struct thread *self = current_thread();
	struct mutex_waiter *waiter;
	irqstate_t flags;

	kassert(preempt_count() == 0);

	flags = spin_lock_irqsave(&mutex->wait_lock);
	if (mutex->owner != self)
		panic("mutex_unlock: name=%s owner=%p(tid=%d) self=%p(tid=%d) "
		      "caller=%p\n",
		      mutex->name, mutex->owner,
		      mutex->owner ? mutex->owner->tid : -1, self, self->tid,
		      __builtin_return_address(0));

	if (!list_empty(&mutex->wait_list)) {
		waiter = list_first_entry(&mutex->wait_list,
					  struct mutex_waiter, node);
		list_del_init(&waiter->node);
		waiter->granted = true;
		mutex->owner = waiter->thread;
		thread_wake(waiter->thread);
	} else {
		mutex->owner = NULL;
	}

	spin_unlock_irqrestore(&mutex->wait_lock, flags);
}

bool mutex_is_locked(const mutex_t *mutex)
{
	return __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) != NULL;
}

bool mutex_is_locked_by_me(const mutex_t *mutex)
{
	return __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) ==
	       current_thread();
}
