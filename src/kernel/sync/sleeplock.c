#include <brk/panic.h>
#include <brk/sleeplock.h>
#include <brk/spinlock.h>
#include <brk/task.h>

void sleeplock_init(sleeplock_t *lock, const char *name)
{
	lock->locked = 0;
	spinlock_init(&lock->lock, "sleeplock");
	lock->pid = -1;
	lock->name = name;
}

void sleeplock_acquire(sleeplock_t *lock)
{
	if (sleeplock_holding(lock))
		panic("sleeplock_acquire: name=%s,pid=%ld\n", lock->name,
		      lock->pid);
	irqstate_t flags = spin_lock_irqsave(&lock->lock);

	while (lock->locked)
		task_sleep(lock, &lock->lock);
	lock->locked = 1;
	lock->pid = current_task()->pid;
	spin_unlock_irqrestore(&lock->lock, flags);
}

void sleeplock_release(sleeplock_t *lock)
{
	if (!sleeplock_holding(lock))
		panic("sleeplock_release: name=%s,pid=%ld\n", lock->name,
		      lock->pid);
	irqstate_t flags = spin_lock_irqsave(&lock->lock);

	lock->locked = 0;
	lock->pid = -1;
	task_wake_all(lock);
	spin_unlock_irqrestore(&lock->lock, flags);
}

bool sleeplock_holding(sleeplock_t *lock)
{
	bool holding;
	irqstate_t flags = spin_lock_irqsave(&lock->lock);

	holding = lock->locked && (current_task()->pid == lock->pid);
	spin_unlock_irqrestore(&lock->lock, flags);
	return holding;
}
