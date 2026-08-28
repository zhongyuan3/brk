#include <arch/pgtable.h>
#include <brk/base/kernel.h>
#include <brk/lib/string.h>
#include <brk/mm/kmalloc.h>
#include <brk/mm/pgalloc.h>
#include <brk/mm/vmalloc.h>
#include <brk/printk/printk.h>
#include <brk/process/signal.h>
#include <brk/process/task.h>
#include <brk/syscall/syscall.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/resource.h>
#include <uapi/brk/time.h>

uint64_t sys_brk(void)
{
	uint64_t addr = syscall_arg_raw(0);
	if (addr == 0)
		return current_task()->mm->brk;
	return task_set_brk(addr);
}

uint64_t sys_clone(void)
{
	return -ENOSYS;
}

uint64_t sys_wait4(void)
{
	pid_t pid = syscall_arg_raw(0);
	int *wstatus = (int *)syscall_arg_raw(1);
	int opts = syscall_arg_raw(2);
	struct rusage *rus = (struct rusage *)syscall_arg_raw(3);
	return task_wait(pid, wstatus, opts, rus);
}

uint64_t sys_fork(void)
{
	return task_fork();
}

uint64_t sys_exit(void)
{
	int status = syscall_arg_raw(0);

	task_exit_normal(status);
	return 0;
}

uint64_t sys_kill(void)
{
	pid_t pid = syscall_arg_int(0);
	int sig = syscall_arg_int(1);

	return signal_do_kill(pid, sig);
}

uint64_t sys_sched_yield(void)
{
	task_yield();
	return 0;
}

uint64_t sys_getpid(void)
{
	return current_task()->tgid;
}

uint64_t sys_getppid(void)
{
	struct task_control_block *par = current_task()->parent;

	return par ? par->tgid : 0;
}

uint64_t sys_gettid(void)
{
	return current_task()->pid;
}
