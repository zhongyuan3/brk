#ifndef _BRK_LOCKDEP_H
#define _BRK_LOCKDEP_H

#include <brk/lockdep_types.h>
#include <brk/types.h>
#include <stdbool.h>

#ifndef NDEBUG

void lockdep_init(void);
void lockdep_acquire(const char *name, const void *lock, enum lockdep_mode mode,
		     bool trylock, void *caller);
void lockdep_release(const void *lock);
bool lockdep_is_held(const void *lock);

#else

#define lockdep_init() ((void)0)
#define lockdep_acquire(name, lock, mode, trylock, caller) ((void)0)
#define lockdep_release(lock) ((void)0)
#define lockdep_is_held(lock) false

#endif

#endif
