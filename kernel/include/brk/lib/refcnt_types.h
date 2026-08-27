#ifndef BRK_REFCNT_TYPES_H
#define BRK_REFCNT_TYPES_H

#include <brk/base/types.h>
#include <brk/lock/spinlock_types.h>

typedef unsigned int refcnt_value_t;

typedef struct __refcnt {
	refcnt_value_t counter;
	spinlock_t lock;
} refcnt_t;

#endif
