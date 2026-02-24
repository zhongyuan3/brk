#ifndef AOSD_SPINLOCK_H
#define AOSD_SPINLOCK_H

#include <aosd/lock.h>
#include <aosd/types.h>

void spinlock_init(spinlock_t *lock, const char *name);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
bool spinlock_holding(spinlock_t *lock);

#endif
