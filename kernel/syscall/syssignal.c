#include <brk/kernel.h>
#include <brk/signal.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <uapi/brk/errno.h>
#include <uapi/signal.h>

u64 sys_rt_sigaction(void)
{
	int sig = syscall_arg_int(0);
	const struct sigaction *act = syscall_arg_ptr(1);
	struct sigaction *oact = syscall_arg_ptr(2);
	size_t sigsetsize = syscall_arg_raw(3);

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	return signal_do_sigaction(current_task(), sig, act, oact);
}

u64 sys_rt_sigprocmask(void)
{
	int how = syscall_arg_int(0);
	const sigset_t *set = syscall_arg_ptr(1);
	sigset_t *oldset = syscall_arg_ptr(2);
	size_t sigsetsize = syscall_arg_raw(3);

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	return signal_do_sigprocmask(current_task(), how, set, oldset);
}

u64 sys_rt_sigreturn(void)
{
	return signal_do_sigreturn(current_task());
}
