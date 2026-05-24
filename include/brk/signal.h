#ifndef BRK_SIGNAL_H
#define BRK_SIGNAL_H

#include <brk/process.h>
#include <brk/types.h>
#include <uapi/types.h>

struct user_sigframe {
	struct trap_frame tf;
	u64 blocked;
	int signo;
	int pad;
};

int proc_kill(pid_t pid, int sig);
void proc_send_signal(struct process *proc, int sig);
void proc_signal_init(struct process *proc);
void proc_signal_fork(struct process *child, struct process *parent);
void proc_signal_exec(struct process *proc);
bool proc_signal_pending(struct process *proc);
void proc_deliver_pending(struct process *proc);
u64 proc_do_sigreturn(struct process *proc);
int proc_sigaction(struct process *proc, int sig, const struct sigaction *act,
		   struct sigaction *oact);
int proc_sigprocmask(struct process *proc, int how, const sigset_t *set,
		     sigset_t *oldset);

#endif
