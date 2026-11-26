#ifndef _BRK_RWSEM_TYPES_H
#define _BRK_RWSEM_TYPES_H

#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <brk/wait_types.h>

struct thread;

/*
 * Sleepable reader-writer semaphore.  Readers share the lock; a writer is
 * exclusive.  Waiting writers block new readers so a writer cannot be starved
 * by a continuous stream of readers.
 */
typedef struct rwsem {
	unsigned int readers;
	struct thread *writer;
	unsigned int waiting_writers;
	spinlock_t wait_lock;
	wait_queue_head_t wait;
	const char *name;
} rwsem_t;

#endif
