#ifndef BRK_SLEEPLOCK_TYPES_H
#define BRK_SLEEPLOCK_TYPES_H

#include <brk/base/types.h>
#include <brk/lock/spinlock_types.h>

typedef struct sleeplock {
	volatile unsigned int locked;
	spinlock_t lock;

	/* For debuging: */
	pid_t pid; /* Process holding the lock. */
	const char *name; /* Name of the lock. */
} sleeplock_t;

#define SLEEPLOCK_INITIALIZER(name) \
	{ 0, SPINLOCK_INITIALIZER("sleeplock"), -1, name }
#define SLEEPLOCK_DEFINE(name) sleeplock_t name = SLEEPLOCK_INITIALIZER(#name)
#define SLEEPLOCK_DECLARE(name) sleeplock_t name

#endif
