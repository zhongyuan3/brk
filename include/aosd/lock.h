#ifndef AOSD_LOCK_H
#define AOSD_LOCK_H

struct cpu;
struct task;

typedef struct {
	volatile unsigned int locked;

	/* For debuging: */
	struct cpu *cpu; /* CPU holding the lock. */
	const char *name; /* Name of the lock. */
} spinlock_t;

#define spinlock_initializer(name) { 0, NULL, name }
#define spinlock_define(name) spinlock_t name = spinlock_initializer(#name)

typedef struct {
	volatile unsigned int locked;
	spinlock_t lock;

	/* For debuging: */
	struct task *task; /* Task holding the lock. */
	const char *name; /* Name of the lock. */
} sleeplock_t;

#define sleeplock_initializer(name) \
	{ 0, spinlock_initializer("sleeplock"), NULL, name }

#endif
