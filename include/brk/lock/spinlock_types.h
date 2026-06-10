#ifndef BRK_SPINLOCK_TYPES_H
#define BRK_SPINLOCK_TYPES_H

#include <brk/lib/types.h>

typedef struct spinlock {
	volatile unsigned int locked;

	/* For debuging: */
	cpuid_t cpuid; /* CPU holding the lock. */
	const char *name; /* Name of the lock. */
} spinlock_t;

#define SPINLOCK_INITIALIZER(name) { 0, -1, name }
#define SPINLOCK_DEFINE(name) spinlock_t name = SPINLOCK_INITIALIZER(#name)
#define SPINLOCK_DECLARE(name) spinlock_t name

#endif
