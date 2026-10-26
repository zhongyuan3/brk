#include <brk/panic.h>
#include <brk/spinlock.h>
#include <brk/task.h>

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
		panic("spinlock_acquire: name=%s,cpu=%lu\n", lock->name,
		      lock->cpuid);
	while (__sync_lock_test_and_set(&lock->locked, 1) != 0)
		;
	__sync_synchronize();
	lock->cpuid = this_cpuid();
}

bool spinlock_tryacquire(spinlock_t *lock)
{
	push_off();
	if (spinlock_holding(lock))
		panic("spinlock_tryacquire: name=%s,cpu=%lu\n", lock->name,
		      lock->cpuid);
	if (__sync_lock_test_and_set(&lock->locked, 1) != 0) {
		pop_off();
		return false;
	}
	__sync_synchronize();
	lock->cpuid = this_cpuid();
	return true;
}

void spinlock_release(spinlock_t *lock)
{
	if (!spinlock_holding(lock))
		panic("spinlock_release: name=%s,cpu=%lu\n", lock->name,
		      lock->cpuid);
	lock->cpuid = -1;
	__sync_synchronize();
	__sync_lock_release(&lock->locked);
	pop_off();
}

bool spinlock_holding(spinlock_t *lock)
{
	return lock->locked && lock->cpuid == this_cpuid();
}
