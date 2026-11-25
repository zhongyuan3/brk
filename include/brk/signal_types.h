#ifndef BRK_SIGNAL_TYPES_H
#define BRK_SIGNAL_TYPES_H

#include <brk/spinlock_types.h>
#include <uapi/signal.h>

struct __brk_sigaction_table {
	struct __brk_sigaction actions[NSIG];
	spinlock_t lock;
};

#endif
