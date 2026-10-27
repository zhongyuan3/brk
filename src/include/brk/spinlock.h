#ifndef BRK_SPINLOCK_H
#define BRK_SPINLOCK_H

#include <arch/irqstate.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

/*
 * Plain spinlock: mutual exclusion plus preemption disable. It does not
 * touch the interrupt state.
 *
 * If the protected data can also be accessed from an interrupt handler, or
 * if the critical section may block (task_sleep/task_yield/task_exit), use
 * the irqsave variants below instead. The irqsave variants do not affect
 * preempt_count: interrupts are already off, and they may be released and
 * reacquired around a task_sleep().
 */
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
bool spin_trylock(spinlock_t *lock);
bool spin_is_locked_by_me(spinlock_t *lock);

/*
 * Raw variants do not change preempt_count. They are only for the
 * scheduler's internal handoff locks, which are held across a context
 * switch where preempt_count must stay zero.
 */
void raw_spin_lock(spinlock_t *lock);
void raw_spin_unlock(spinlock_t *lock);

/*
 * Interrupt-safe spinlock. The caller owns @flags (normally a stack
 * variable); this keeps the interrupt state per-task and makes nesting
 * work without any global bookkeeping.
 *
 * Interrupts are already disabled here, so these variants deliberately do
 * not touch preempt_count. This also lets them be held across task_sleep()
 * without preventing the switch itself.
 *
 *	irqstate_t flags = spin_lock_irqsave(&lock);
 *	...
 *	spin_unlock_irqrestore(&lock, flags);
 */
irqstate_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, irqstate_t flags);
bool spin_trylock_irqsave(spinlock_t *lock, irqstate_t *flags);

void spinlock_init(spinlock_t *lock, const char *name);

#endif
