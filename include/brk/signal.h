#ifndef BRK_SIGNAL_H
#define BRK_SIGNAL_H

#include <brk/types.h>
#include <uapi/types.h>

struct process;

int proc_kill(pid_t pid, int sig);
void proc_send_signal(struct process *proc, int sig);
void proc_deliver_fatal(struct process *proc);

#endif
