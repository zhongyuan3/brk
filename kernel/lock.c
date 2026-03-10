#include <aosd/cpu.h>
#include <aosd/lock.h>
#include <aosd/panic.h>
#include <aosd/process.h>

void spinlock_init(spinlock_t *lock, const char *name)
{
	lock->locked = 0;
	lock->cpu = NULL;
	lock->name = name;
}

void spinlock_acquire(spinlock_t *lock)
{
	push_off();
	if (spinlock_holding(lock))
		panic("spinlock_acquire: name=%s,cpu=%u\n", lock->name,
		      lock->cpu->hart_id);
	while (__sync_lock_test_and_set(&lock->locked, 1) != 0)
		;
	__sync_synchronize();
	lock->cpu = current_cpu();
}

void spinlock_release(spinlock_t *lock)
{
	if (!spinlock_holding(lock))
		panic("spinlock_release: name=%s,cpu=%u\n", lock->name,
		      lock->cpu->hart_id);
	lock->cpu = NULL;
	__sync_synchronize();
	__sync_lock_release(&lock->locked);
	pop_off();
}

bool spinlock_holding(spinlock_t *lock)
{
	return lock->locked && lock->cpu == current_cpu();
}

void sleeplock_init(sleeplock_t *lock, const char *name)
{
	lock->locked = 0;
	spinlock_init(&lock->lock, "sleeplock");
	lock->proc = NULL;
	lock->name = name;
}

void sleeplock_acquire(sleeplock_t *lock)
{
	spinlock_acquire(&lock->lock);
	while (lock->locked)
		proc_sleep(lock, &lock->lock);
	lock->locked = 1;
	lock->proc = current_cpu()->current;
	spinlock_release(&lock->lock);
}

void sleeplock_release(sleeplock_t *lock)
{
	spinlock_acquire(&lock->lock);
	lock->locked = 0;
	lock->proc = NULL;
	proc_wake_up(lock);
	spinlock_release(&lock->lock);
}

bool sleeplock_holding(sleeplock_t *lock)
{
	bool holding;

	spinlock_acquire(&lock->lock);
	holding = lock->locked && (current_cpu()->current == lock->proc);
	spinlock_release(&lock->lock);
	return holding;
}
