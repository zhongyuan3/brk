#ifndef _BRK_MUTEX_TYPES_H
#define _BRK_MUTEX_TYPES_H

#include <brk/list.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>

struct thread;

/*
 * Sleepable mutex.  The fast path is a single atomic compare-and-exchange on
 * @owner; the slow path queues a waiter and sleeps on the mutex channel.
 * Ownership is tracked by thread pointer so recursive locking and unlock by a
 * non-owner can be detected.
 */
typedef struct mutex {
	struct thread *owner;
	spinlock_t wait_lock;
	struct list_head wait_list;
	const char *name;
} mutex_t;

#define __MUTEX_INITIALIZER(var)                                      \
	{                                                             \
		.owner = NULL,                                        \
		.wait_lock = SPINLOCK_INITIALIZER("mutex.wait_lock"), \
		.wait_list = LIST_INITIALIZER((var).wait_list),       \
		.name = #var,                                         \
	}

#define MUTEX_DEFINE(name) mutex_t name = __MUTEX_INITIALIZER(name)

#endif
