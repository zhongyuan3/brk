#ifndef BRK_SPINLOCK_H
#define BRK_SPINLOCK_H

#include <brk/base/types.h>
#include <brk/lock/spinlock_types.h>

void spinlock_init(spinlock_t *lock, const char *name);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
bool spinlock_holding(spinlock_t *lock);

#endif
