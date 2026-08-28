#include <brk/base/kernel.h>
#include <brk/process/signal.h>
#include <brk/process/task.h>
#include <brk/syscall/syscall.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/signal.h>

uint64_t sys_rt_sigaction(void)
{
	int sig = syscall_arg_int(0);
	const struct sigaction *act = syscall_arg_ptr(1);
	struct sigaction *oact = syscall_arg_ptr(2);
	size_t sigsetsize = syscall_arg_raw(3);

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	return signal_do_sigaction(current_task(), sig, act, oact);
}

uint64_t sys_rt_sigprocmask(void)
{
	int how = syscall_arg_int(0);
	const sigset_t *set = syscall_arg_ptr(1);
	sigset_t *oldset = syscall_arg_ptr(2);
	size_t sigsetsize = syscall_arg_raw(3);

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	return signal_do_sigprocmask(current_task(), how, set, oldset);
}

uint64_t sys_rt_sigreturn(void)
{
	return signal_do_sigreturn(current_task());
}
