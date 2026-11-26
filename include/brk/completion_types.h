#ifndef _BRK_COMPLETION_TYPES_H
#define _BRK_COMPLETION_TYPES_H

#include <brk/spinlock_types.h>
#include <brk/types.h>

/*
 * One-shot completion event.  complete()/complete_all() may be called from
 * interrupt context; wait_for_completion() must only be called from thread
 * context.
 */
typedef struct completion {
	unsigned int done;
	spinlock_t wait_lock;
	const char *name;
} completion_t;

#define __COMPLETION_INITIALIZER(var)                                      \
	{                                                                  \
		.done = 0,                                                 \
		.wait_lock = SPINLOCK_INITIALIZER("completion.wait_lock"), \
		.name = #var,                                              \
	}

#define COMPLETION_DEFINE(name) \
	completion_t name = __COMPLETION_INITIALIZER(name)

#endif
