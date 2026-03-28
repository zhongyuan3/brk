#include <aosd/lock.h>
#include <aosd/panic.h>
#include <aosd/process.h>

void spinlock_init(spinlock_t *lock, const char *name)
{
	lock->locked = 0;
	lock->cpuid = -1;
	lock->name = name;
}

void spinlock_acquire(spinlock_t *lock)
{
	push_off();
	if (spinlock_holding(lock))
		panic("spinlock_acquire: name=%s,cpu=%d\n", lock->name,
		      lock->cpuid);
	while (__sync_lock_test_and_set(&lock->locked, 1) != 0)
		;
	__sync_synchronize();
	lock->cpuid = current_cpuid();
}

void spinlock_release(spinlock_t *lock)
{
	if (!spinlock_holding(lock))
		panic("spinlock_release: name=%s,cpu=%d\n", lock->name,
		      lock->cpuid);
	lock->cpuid = -1;
	__sync_synchronize();
	__sync_lock_release(&lock->locked);
	pop_off();
}

bool spinlock_holding(spinlock_t *lock)
{
	return lock->locked && lock->cpuid == current_cpuid();
}

void sleeplock_init(sleeplock_t *lock, const char *name)
{
	lock->locked = 0;
	spinlock_init(&lock->lock, "sleeplock");
	lock->pid = -1;
	lock->name = name;
}

void sleeplock_acquire(sleeplock_t *lock)
{
	spinlock_acquire(&lock->lock);
	while (lock->locked)
		proc_sleep(lock, &lock->lock);
	lock->locked = 1;
	lock->pid = current_process()->pid;
	spinlock_release(&lock->lock);
}

void sleeplock_release(sleeplock_t *lock)
{
	spinlock_acquire(&lock->lock);
	lock->locked = 0;
	lock->pid = -1;
	proc_wake_up(lock);
	spinlock_release(&lock->lock);
}

bool sleeplock_holding(sleeplock_t *lock)
{
	bool holding;

	spinlock_acquire(&lock->lock);
	holding = lock->locked && (current_process()->pid == lock->pid);
	spinlock_release(&lock->lock);
	return holding;
}
