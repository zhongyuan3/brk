#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/kernel.h>
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
		deadline = ktime_get_boot_ns() + (u64)timeout_ms * 1000000ULL;

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
		if (!infinite && ktime_get_boot_ns() >= deadline)
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

u64 sys_poll(void)
{
	return poll_common(syscall_arg_ptr(0), (nfds_t)syscall_arg_raw(1),
			   syscall_arg_int(2));
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

static int timeval_to_ms(const struct timeval *tv)
{
	u64 ms;

	if (tv->tv_sec < 0 || tv->tv_usec < 0)
		return -EINVAL;

	ms = (u64)tv->tv_sec * 1000 + ((u64)tv->tv_usec + 999) / 1000;
	if (ms > 0x7fffffffULL)
		return 0x7fffffff;
	return (int)ms;
}

static u64 select_common(int nfds, fd_set *uread, fd_set *uwrite,
			 fd_set *uexcept, int timeout_ms)
{
	fd_set rset, wset, eset, rout, wout, eout;
	struct pollfd *fds = NULL;
	unsigned int cnt = 0;
	int ret;

	if (nfds < 0 || nfds > OPEN_MAX)
		return -EINVAL;

	memset(&rset, 0, sizeof(rset));
	memset(&wset, 0, sizeof(wset));
	memset(&eset, 0, sizeof(eset));

	if (uread && copy_from_user(&rset, uread, sizeof(fd_set)))
		return -EFAULT;
	if (uwrite && copy_from_user(&wset, uwrite, sizeof(fd_set)))
		return -EFAULT;
	if (uexcept && copy_from_user(&eset, uexcept, sizeof(fd_set)))
		return -EFAULT;

	if (nfds) {
		fds = kcalloc((size_t)nfds, sizeof(*fds));
		if (!fds)
			return -ENOMEM;

		for (int i = 0; i < nfds; i++) {
			short ev = 0;

			if (uread && FD_ISSET(i, &rset))
				ev |= POLLIN;
			if (uwrite && FD_ISSET(i, &wset))
				ev |= POLLOUT;
			if (uexcept && FD_ISSET(i, &eset))
				ev |= POLLPRI;
			if (!ev)
				continue;

			fds[cnt].fd = i;
			fds[cnt].events = ev;
			cnt++;
		}
	}

	ret = do_poll(fds, cnt, timeout_ms);
	if (ret < 0) {
		kfree(fds);
		return ret;
	}

	for (unsigned int i = 0; i < cnt; i++) {
		if (fds[i].revents & POLLNVAL) {
			kfree(fds);
			return -EBADF;
		}
	}

	memset(&rout, 0, sizeof(rout));
	memset(&wout, 0, sizeof(wout));
	memset(&eout, 0, sizeof(eout));

	for (unsigned int i = 0; i < cnt; i++) {
		int fd = fds[i].fd;

		if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
			FD_SET(fd, &rout);
		if (fds[i].revents & (POLLOUT | POLLERR))
			FD_SET(fd, &wout);
		if (fds[i].revents & POLLPRI)
			FD_SET(fd, &eout);
	}

	if (uread && copy_to_user(uread, &rout, sizeof(fd_set)))
		ret = -EFAULT;
	if (uwrite && copy_to_user(uwrite, &wout, sizeof(fd_set)))
		ret = -EFAULT;
	if (uexcept && copy_to_user(uexcept, &eout, sizeof(fd_set)))
		ret = -EFAULT;

	kfree(fds);
	return ret;
}

u64 sys_select(void)
{
	int nfds = syscall_arg_int(0);
	fd_set *uread = syscall_arg_ptr(1);
	fd_set *uwrite = syscall_arg_ptr(2);
	fd_set *uexcept = syscall_arg_ptr(3);
	struct timeval *utimeout = syscall_arg_ptr(4);
	int timeout_ms = -1;

	if (utimeout) {
		struct timeval tv;

		if (copy_from_user(&tv, utimeout, sizeof(tv)))
			return -EFAULT;
		timeout_ms = timeval_to_ms(&tv);
		if (timeout_ms < 0)
			return -EINVAL;
	}

	return select_common(nfds, uread, uwrite, uexcept, timeout_ms);
}

u64 sys_pselect(void)
{
	int nfds = syscall_arg_int(0);
	fd_set *uread = syscall_arg_ptr(1);
	fd_set *uwrite = syscall_arg_ptr(2);
	fd_set *uexcept = syscall_arg_ptr(3);
	const struct timespec *utimeout = syscall_arg_ptr(4);
	const sigset_t *usigmask = syscall_arg_ptr(5);
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

	ret = (int)select_common(nfds, uread, uwrite, uexcept, timeout_ms);

	if (applied > 0)
		signal_do_sigprocmask(current_task(), SIG_SETMASK, &oldmask,
				      NULL);
	return ret;
}
