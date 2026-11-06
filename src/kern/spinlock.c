#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/task_types.h>

void spinlock_init(spinlock_t *lock, const char *name)
{
	lock->locked = 0;
	lock->cpuid = -1;
	lock->name = name;
}

bool spin_is_locked_by_me(spinlock_t *lock)
{
	return lock->locked && lock->cpuid == this_cpuid();
}

static void __spin_lock(spinlock_t *lock)
{
	while (__sync_lock_test_and_set(&lock->locked, 1) != 0)
		;
	__sync_synchronize();
	lock->cpuid = this_cpuid();
}

static bool __spin_trylock(spinlock_t *lock)
{
	if (__sync_lock_test_and_set(&lock->locked, 1) != 0)
		return false;
	__sync_synchronize();
	lock->cpuid = this_cpuid();
	return true;
}

static void __spin_unlock(spinlock_t *lock, bool check_owner)
{
	if (check_owner && !spin_is_locked_by_me(lock))
		panic("spin_unlock: name=%s,cpu=%lu\n", lock->name,
		      lock->cpuid);
	lock->cpuid = -1;
	__sync_synchronize();
	__sync_lock_release(&lock->locked);
}

void raw_spin_lock(spinlock_t *lock)
{
	__spin_lock(lock);
}

void raw_spin_unlock(spinlock_t *lock)
{
	__spin_unlock(lock, false);
}

void spin_lock(spinlock_t *lock)
{
	preempt_disable();
	__spin_lock(lock);
}

bool spin_trylock(spinlock_t *lock)
{
	preempt_disable();
	if (!__spin_trylock(lock)) {
		preempt_enable();
		return false;
	}
	return true;
}

void spin_unlock(spinlock_t *lock)
{
	__spin_unlock(lock, true);
	preempt_enable();
}

irqstate_t spin_lock_irqsave(spinlock_t *lock)
{
	irqstate_t flags = local_irq_save();

	__spin_lock(lock);
	return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, irqstate_t flags)
{
	__spin_unlock(lock, true);
	local_irq_restore(flags);
}

bool spin_trylock_irqsave(spinlock_t *lock, irqstate_t *flags)
{
	irqstate_t saved = local_irq_save();

	if (!__spin_trylock(lock)) {
		local_irq_restore(saved);
		return false;
	}

	*flags = saved;
	return true;
}
