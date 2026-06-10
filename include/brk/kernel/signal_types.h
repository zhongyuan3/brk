#ifndef BRK_SIGNAL_TYPES_H
#define BRK_SIGNAL_TYPES_H

#include <arch/trapframe.h>
#include <brk/kernel/refcnt_types.h>
#include <brk/lock/spinlock_types.h>
#include <uapi/signal.h>

struct sigaction_table {
	struct sigaction actions[NSIG];
	spinlock_t lock;
	refcnt_t refcnt;
};

struct user_sigframe {
	struct trap_frame tf;
	u64 blocked;
	int signo;
	int pad;
};

#endif
