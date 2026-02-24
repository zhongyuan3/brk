#ifndef AOSD_SLEEPLOCK_H
#define AOSD_SLEEPLOCK_H

#include <aosd/lock.h>
#include <aosd/types.h>

void sleeplock_init(sleeplock_t *lock, const char *name);
void sleeplock_acquire(sleeplock_t *lock);
void sleeplock_release(sleeplock_t *lock);
bool sleeplock_holding(sleeplock_t *lock);

#endif
