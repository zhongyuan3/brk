#ifndef BRK_SIGNAL_TYPES_H
#define BRK_SIGNAL_TYPES_H

#include <arch/trapframe.h>
#include <brk/lib/refcnt_types.h>
#include <brk/lock/spinlock_types.h>
#include <uapi/brk/signal.h>

struct sigaction_table {
	struct sigaction actions[NSIG];
	spinlock_t lock;
	refcnt_t refcnt;
};

struct user_sigframe {
	struct trap_frame tf;
	uint64_t blocked;
	int signo;
	int pad;
};

#endif
