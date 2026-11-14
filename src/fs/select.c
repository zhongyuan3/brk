#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/ktime.h>
#include <brk/signal.h>
#include <brk/string.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/timeconst.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/limits.h>
#include <uapi/poll.h>
#include <uapi/select.h>
#include <uapi/signal.h>
#include <uapi/time.h>

#define POLL_SLEEP_NS 10000000ULL

static int do_poll(struct pollfd *fds, unsigned int nfds, int timeout_ms)
{
	bool infinite = timeout_ms < 0;
	u64 deadline = 0;

	if (!infinite)
		deadline = ktime_boottime_ns() + (u64)timeout_ms * 1000000ULL;

	for (;;) {
		unsigned int ready = 0;

		for (unsigned int i = 0; i < nfds; i++) {
			struct fs_file *file;
			unsigned int rev;

			fds[i].revents = 0;
			if (fds[i].fd < 0)
				continue;

			file = fdtable_get_file(current_task()->fdtable,
						fds[i].fd);
			if (IS_ERR(file)) {
				fds[i].revents = POLLNVAL;
				ready++;
				continue;
			}

			rev = fs_file_poll(file, (unsigned int)fds[i].events);
			fs_file_put(file);

			fds[i].revents =
				(short)(rev & ((unsigned int)fds[i].events |
					       POLLERR | POLLHUP | POLLNVAL));
			if (fds[i].revents)
				ready++;
		}

		if (ready)
			return (int)ready;
		if (!infinite && ktime_boottime_ns() >= deadline)
			return 0;
		if (task_is_killed(current_task()))
			return -EINTR;

		ktime_nanosleep((ktime_t)POLL_SLEEP_NS, NULL);
	}
}

static u64 poll_common(struct pollfd *ufds, nfds_t nfds, int timeout_ms)
{
	struct pollfd *fds = NULL;
	size_t bytes = 0;
	int ret;

	if (nfds > OPEN_MAX)
		return -EINVAL;
	if (nfds && !ufds)
		return -EFAULT;

	if (nfds) {
		bytes = (size_t)nfds * sizeof(*fds);
		fds = kmalloc(bytes);
		if (!fds)
			return -ENOMEM;
		if (copy_from_user(fds, ufds, bytes)) {
			kfree(fds);
			return -EFAULT;
		}
	}

	ret = do_poll(fds, (unsigned int)nfds, timeout_ms);

	if (ret >= 0 && nfds && copy_to_user(ufds, fds, bytes))
		ret = -EFAULT;

	kfree(fds);
	return ret;
}

static int timespec_to_ms(const struct timespec *ts, int *ms)
{
	u64 v;

	if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
	    ts->tv_nsec >= (__kernel_long_t)NS_PER_SEC)
		return -EINVAL;

	v = (u64)ts->tv_sec * MS_PER_SEC;
	v += ((u64)ts->tv_nsec + NS_PER_MS - 1) / NS_PER_MS;
	if (v > 0x7fffffffULL)
		v = 0x7fffffffULL;
	*ms = (int)v;
	return 0;
}

static int apply_sigmask(const sigset_t *uset, sigset_t *oldmask)
{
	sigset_t mask;

	if (!uset)
		return 0;
	if (copy_from_user(&mask, uset, sizeof(mask)))
		return -EFAULT;
	if (signal_do_sigprocmask(current_task(), SIG_SETMASK, &mask, oldmask))
		return -EINVAL;
	return 1;
}

u64 sys_ppoll(void)
{
	struct pollfd *ufds = syscall_arg_ptr(0);
	nfds_t nfds = (nfds_t)syscall_arg_raw(1);
	const struct timespec *utimeout = syscall_arg_ptr(2);
	const sigset_t *usigmask = syscall_arg_ptr(3);
	struct timespec ts;
	sigset_t oldmask;
	int timeout_ms = -1;
	int applied;
	int ret;

	if (utimeout) {
		if (copy_from_user(&ts, utimeout, sizeof(ts)))
			return -EFAULT;
		ret = timespec_to_ms(&ts, &timeout_ms);
		if (ret)
			return ret;
	}

	applied = apply_sigmask(usigmask, &oldmask);
	if (applied < 0)
		return applied;

	ret = (int)poll_common(ufds, nfds, timeout_ms);

	if (applied > 0)
		signal_do_sigprocmask(current_task(), SIG_SETMASK, &oldmask,
				      NULL);
	return ret;
}
