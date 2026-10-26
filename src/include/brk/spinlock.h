#ifndef BRK_SPINLOCK_H
#define BRK_SPINLOCK_H

// #include <brk/cpu.h>
// #include <brk/kernel.h>
// #include <brk/printk.h>
#include <brk/spinlock_types.h>
// #include <brk/string.h>
// #include <brk/task_types.h>
#include <brk/types.h>

void spinlock_init(spinlock_t *lock, const char *name);
void spinlock_acquire(spinlock_t *lock);
bool spinlock_tryacquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
bool spinlock_holding(spinlock_t *lock);

/*
#ifndef SPINLOCK_IMPL

#define spinlock_acquire(l)                                                    \
	do {                                                                   \
		spinlock_t *_l = (l);                                          \
		if (!strcmp(_l->name, "task")) {                               \
			struct task *_t = container_of(_l, struct task, lock); \
			pr_debug("%s:%d:%s(): acquire spin task@%ld\n",        \
				 __FILE__, __LINE__, __func__, _t->pid);       \
		}                                                              \
		spinlock_acquire(_l);                                          \
	} while (0)

#define spinlock_release(l)                                                    \
	do {                                                                   \
		spinlock_t *_l = (l);                                          \
		if (!strcmp(_l->name, "task")) {                               \
			struct task *_t = container_of(_l, struct task, lock); \
			pr_debug("%s:%d:%s(): release spin task@%ld\n",        \
				 __FILE__, __LINE__, __func__, _t->pid);       \
		}                                                              \
		spinlock_release(_l);                                          \
	} while (0)

#endif
*/

#endif
