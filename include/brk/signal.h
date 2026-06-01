#ifndef BRK_SIGNAL_H
#define BRK_SIGNAL_H

#include <brk/refcnt_types.h>
#include <brk/signal_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <uapi/signal.h>
#include <uapi/types.h>

struct sigaction_table *sigaction_table_alloc(void);
void sigaction_table_put(struct sigaction_table *table);
struct sigaction_table *sigaction_table_get(struct sigaction_table *table);
void sigaction_table_copy(struct sigaction_table *dst,
			  struct sigaction_table *src);
void sigaction_table_reset(struct sigaction_table *table);

void signal_send(struct task_control_block *task, int sig);
int signal_init(struct task_control_block *task);
void signal_deinit(struct task_control_block *task);
void signal_reset(struct task_control_block *task);
bool signal_pending(struct task_control_block *task);
void signal_deliver_pending(struct task_control_block *task);
void signal_copy(struct task_control_block *dst,
		 struct task_control_block *src);

int signal_do_kill(pid_t pid, int sig);
u64 signal_do_sigreturn(struct task_control_block *task);
int signal_do_sigaction(struct task_control_block *task, int sig,
			const struct sigaction *act, struct sigaction *oact);
int signal_do_sigprocmask(struct task_control_block *task, int how,
			  const sigset_t *set, sigset_t *oldset);

#endif
