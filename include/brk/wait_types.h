#ifndef _BRK_WAIT_TYPES_H
#define _BRK_WAIT_TYPES_H

#include <brk/list.h>
#include <brk/spinlock_types.h>

/*
 * A wait queue identifies one wait condition.  @lock is the condition lock
 * that waiters hold across the condition check and wait publication.  It may
 * be shared by several wait queues when one lock protects multiple conditions.
 *
 * @waiters is the list of sleeping threads for this condition.  It is
 * protected by the scheduler's sleep-queue lock, not by @lock; this keeps the
 * condition lock and the scheduler bookkeeping independent.
 */
typedef struct wait_queue_head {
	spinlock_t *lock;
	const char *name;
	struct list_head waiters;
} wait_queue_head_t;

#define WAIT_QUEUE_INITIALIZER(var, guard) \
	{ .lock = (guard),                 \
	  .name = #var,                    \
	  .waiters = LIST_INITIALIZER((var).waiters) }

#define WAIT_QUEUE_DEFINE(var, guard) \
	wait_queue_head_t var = WAIT_QUEUE_INITIALIZER(var, guard)

#endif
