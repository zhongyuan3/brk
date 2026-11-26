#ifndef _BRK_MUTEX_H
#define _BRK_MUTEX_H

#include <brk/mutex_types.h>
#include <brk/types.h>
#include <stdbool.h>

void mutex_init(mutex_t *mutex, const char *name);

/*
 * Uninterruptible mutex acquisition.  Must be called from thread context with
 * preemption enabled.  Acquiring the same mutex recursively panics.
 */
void mutex_lock(mutex_t *mutex);

/*
 * Interruptible acquisition.  Returns 0 when the mutex was acquired, or
 * -EINTR when a signal or process-group exit aborted the wait.
 */
int mutex_lock_interruptible(mutex_t *mutex);

/* Try to acquire the mutex without sleeping. */
bool mutex_trylock(mutex_t *mutex);

/* Release the mutex; panics if the caller is not the owner. */
void mutex_unlock(mutex_t *mutex);

bool mutex_is_locked(const mutex_t *mutex);
bool mutex_is_locked_by_me(const mutex_t *mutex);

#endif
