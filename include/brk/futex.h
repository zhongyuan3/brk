#ifndef _BRK_FUTEX_H
#define _BRK_FUTEX_H

#include <brk/thread_types.h>
#include <brk/types.h>

struct uvm_space;

void futex_init(void);

/* Wake every waiter on @uaddr; used by clear_child_tid on thread exit. */
int futex_wake_user(struct uvm_space *mm, u32 *uaddr, u32 bitset, int nr);
/* Remove a thread's embedded waiter from its hash bucket, if queued. */
void futex_cleanup_thread(struct thread *thread);

#endif
