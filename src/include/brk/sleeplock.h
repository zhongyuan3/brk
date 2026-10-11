#ifndef BRK_SLEEPLOCK_H
#define BRK_SLEEPLOCK_H

#include <brk/sleeplock_types.h>
#include <brk/types.h>

void sleeplock_init(sleeplock_t *lock, const char *name);
void sleeplock_acquire(sleeplock_t *lock);
void sleeplock_release(sleeplock_t *lock);
bool sleeplock_holding(sleeplock_t *lock);

#endif
