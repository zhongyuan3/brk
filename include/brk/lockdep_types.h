#ifndef _BRK_LOCKDEP_TYPES_H
#define _BRK_LOCKDEP_TYPES_H

#include <brk/types.h>

#define LOCKDEP_MAX_HELD 24

enum lockdep_mode {
	LOCKDEP_READ,
	LOCKDEP_WRITE,
};

/*
 * One lock held by a thread.  @lock identifies the instance, @class_id names
 * the abstract lock class used for dependency tracking, and @mode records
 * whether a rwsem was taken for reading or writing.
 */
struct lockdep_held {
	const void *lock;
	u16 class_id;
	u8 mode;
	u8 reserved;
};

#endif
