#include <aosd/cpu.h>
#include <aosd/panic.h>
#include <aosd/sched.h>
#include <aosd/sleeplock.h>
#include <aosd/spinlock.h>

void sleeplock_init(sleeplock_t *lock, const char *name)
{
	lock->locked = 0;
	spinlock_init(&lock->lock, "sleeplock");
	lock->task = NULL;
	lock->name = name;
}

void sleeplock_acquire(sleeplock_t *lock)
{
	spinlock_acquire(&lock->lock);
	while (lock->locked)
		sched_sleep(lock, &lock->lock);
	lock->locked = 1;
	lock->task = current_cpu()->current;
	spinlock_release(&lock->lock);
}

void sleeplock_release(sleeplock_t *lock)
{
	spinlock_acquire(&lock->lock);
	lock->locked = 0;
	lock->task = NULL;
	sched_wake_up(lock);
	spinlock_release(&lock->lock);
}

bool sleeplock_holding(sleeplock_t *lock)
{
	bool holding;

	spinlock_acquire(&lock->lock);
	holding = lock->locked && (current_cpu()->current == lock->task);
	spinlock_release(&lock->lock);
	return holding;
}
