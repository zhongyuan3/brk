#ifndef BRK_SIGNAL_H
#define BRK_SIGNAL_H

#include <brk/signal_types.h>
#include <brk/spinlock_types.h>
#include <brk/types.h>
#include <stdbool.h>
#include <uapi/signal.h>
#include <uapi/types.h>

struct process;
struct thread;

void sigaction_table_init(struct __brk_sigaction_table *table);
void sigaction_table_copy(struct __brk_sigaction_table *dst,
			  struct __brk_sigaction_table *src);
void sigaction_table_reset(struct __brk_sigaction_table *table);

void signal_send(struct thread *thread, int sig);
void signal_send_process(struct process *process, int sig);
void signal_send_pgrp(pid_t pgrp, int sig);
void signal_init(struct process *process, struct thread *thread, u64 blocked);
void signal_thread_init(struct process *process, struct thread *thread,
			u64 blocked);
void signal_reset(struct process *process, struct thread *thread);
bool signal_pending(struct thread *thread);
void signal_deliver_pending(struct thread *thread);
void signal_copy(struct process *dst_process, struct thread *dst_thread,
		 struct process *src_process, struct thread *src_thread);

int signal_do_kill(pid_t pid, int sig);
u64 signal_do_sigreturn(struct thread *thread);
int signal_do_sigaction(struct process *process, int sig,
			const struct __brk_sigaction *act, struct __brk_sigaction *oact);
int signal_do_sigprocmask(struct thread *thread, int how, const sigset_t *set,
			  sigset_t *oldset);

#endif
