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

static void spin_lock_check(spinlock_t *lock)
{
	if (spin_is_locked_by_me(lock)) {
		if (!strcmp(lock->name, "task")) {
			struct task *t = current_task();
			pid_t pid = t ? t->pid : -1;

			panic("%s: name=%s@%ld,cpu=%lu\n", __func__, lock->name,
			      pid, lock->cpuid);
		} else {
			panic("%s: name=%s,cpu=%lu\n", __func__, lock->name,
			      lock->cpuid);
		}
	}
}

static void __spin_lock(spinlock_t *lock)
{
	spin_lock_check(lock);

	while (__sync_lock_test_and_set(&lock->locked, 1) != 0)
		;
	__sync_synchronize();
	lock->cpuid = this_cpuid();
}

static bool __spin_trylock(spinlock_t *lock)
{
	spin_lock_check(lock);

	if (__sync_lock_test_and_set(&lock->locked, 1) != 0)
		return false;
	__sync_synchronize();
	lock->cpuid = this_cpuid();
	return true;
}

static void __spin_unlock(spinlock_t *lock)
{
	if (!spin_is_locked_by_me(lock))
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
	__spin_unlock(lock);
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
	__spin_unlock(lock);
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
	__spin_unlock(lock);
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
