#ifndef AOSD_LOCK_H
#define AOSD_LOCK_H

#include <aosd/types.h>

struct cpu;
struct task;

typedef struct spinlock {
	volatile unsigned int locked;

	/* For debuging: */
	struct cpu *cpu; /* CPU holding the lock. */
	const char *name; /* Name of the lock. */
} spinlock_t;

#define SPINLOCK_INITIALIZER(name) { 0, NULL, name }
#define SPINLOCK_DEFINE(name) spinlock_t name = SPINLOCK_INITIALIZER(#name)

void spinlock_init(spinlock_t *lock, const char *name);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
bool spinlock_holding(spinlock_t *lock);

typedef struct sleeplock {
	volatile unsigned int locked;
	spinlock_t lock;

	/* For debuging: */
	struct task *task; /* Task holding the lock. */
	const char *name; /* Name of the lock. */
} sleeplock_t;

#define SLEEPLOCK_INITIALIZER(name) \
	{ 0, SPINLOCK_INITIALIZER("sleeplock"), NULL, name }
#define SLEEPLOCK_DEFINE(name) sleeplock_t name = SLEEPLOCK_INITIALIZER(#name)

void sleeplock_init(sleeplock_t *lock, const char *name);
void sleeplock_acquire(sleeplock_t *lock);
void sleeplock_release(sleeplock_t *lock);
bool sleeplock_holding(sleeplock_t *lock);

#endif
