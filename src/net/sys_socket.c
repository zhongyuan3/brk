#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/net/socket.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/socket.h>

static int socket_from_fd(int fd, struct fs_file **filep, struct socket **sockp)
{
	struct fs_file *file;

	if (fd < 0)
		return -EBADF;

	file = fdtable_get_file(current_task()->fdtable, fd);
	if (IS_ERR(file))
		return PTR_ERR(file);

	if (file->ops != &socket_file_ops) {
		fs_file_put(file);
		return -ENOTSOCK;
	}

	*filep = file;
	*sockp = file->private_data;
	return 0;
}

u64 sys_socket(void)
{
	int family = syscall_arg_int(0);
	int type = syscall_arg_int(1);
	int protocol = syscall_arg_int(2);
	bool nonblock = (type & SOCK_NONBLOCK) != 0;
	struct fs_file *file;
	struct socket *sock;
	int fd;
	int err;

	type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

	err = sock_create(family, type, protocol, &sock);
	if (err)
		return err;

	sock->nonblock = nonblock;

	file = fs_file_alloc_anon(&socket_file_ops, sock);
	if (IS_ERR(file)) {
		sock_release(sock);
		return PTR_ERR(file);
	}

	fd = fdtable_alloc_fd(current_task()->fdtable, file);
	if (fd < 0) {
		fs_file_put(file);
		return fd;
	}
	return fd;
}

u64 sys_bind(void)
{
	int fd = syscall_arg_int(0);
	const void *uaddr = syscall_arg_ptr(1);
	int addrlen = syscall_arg_int(2);
	struct sockaddr_in sa;
	struct fs_file *file;
	struct socket *sock;
	int err;

	if (!uaddr || addrlen < (int)sizeof(sa))
		return -EINVAL;
	if (copy_from_user(&sa, uaddr, sizeof(sa)))
		return -EFAULT;
	if (sa.sin_family != AF_INET)
		return -EAFNOSUPPORT;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	err = sock->ops->bind(sock, (const struct sockaddr *)&sa, addrlen);
	fs_file_put(file);
	return err;
}

u64 sys_connect(void)
{
	int fd = syscall_arg_int(0);
	const void *uaddr = syscall_arg_ptr(1);
	int addrlen = syscall_arg_int(2);
	struct sockaddr_in sa;
	struct fs_file *file;
	struct socket *sock;
	int err;

	if (!uaddr || addrlen < (int)sizeof(sa))
		return -EINVAL;
	if (copy_from_user(&sa, uaddr, sizeof(sa)))
		return -EFAULT;
	if (sa.sin_family != AF_INET)
		return -EAFNOSUPPORT;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	err = sock->ops->connect(sock, (const struct sockaddr *)&sa, addrlen);
	fs_file_put(file);
	return err;
}

u64 sys_sendto(void)
{
	int fd = syscall_arg_int(0);
	const void *buf = syscall_arg_ptr(1);
	size_t len = syscall_arg_raw(2);
	unsigned int flags = syscall_arg_raw(3);
	const void *uaddr = syscall_arg_ptr(4);
	int addrlen = syscall_arg_int(5);
	struct sockaddr_in sa;
	struct fs_file *file;
	struct socket *sock;
	int err;

	(void)flags;

	if (uaddr) {
		if (addrlen < (int)sizeof(sa))
			return -EINVAL;
		if (copy_from_user(&sa, uaddr, sizeof(sa)))
			return -EFAULT;
		if (sa.sin_family != AF_INET)
			return -EAFNOSUPPORT;
	}

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	err = sock->ops->sendmsg(sock, buf, len,
				 uaddr ? (const struct sockaddr *)&sa : NULL,
				 uaddr ? (int)sizeof(sa) : 0);
	fs_file_put(file);
	return err;
}

u64 sys_recvfrom(void)
{
	int fd = syscall_arg_int(0);
	void *buf = syscall_arg_ptr(1);
	size_t len = syscall_arg_raw(2);
	unsigned int flags = syscall_arg_raw(3);
	void *uaddr = syscall_arg_ptr(4);
	int *uaddrlen = syscall_arg_ptr(5);
	struct sockaddr_in sa;
	struct fs_file *file;
	struct socket *sock;
	int salen = sizeof(sa);
	int ulen = 0;
	int err;

	(void)flags;

	if (uaddr && uaddrlen) {
		if (copy_from_user(&ulen, uaddrlen, sizeof(ulen)))
			return -EFAULT;
		if (ulen < 0)
			ulen = 0;
	}

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	err = sock->ops->recvmsg(sock, buf, len,
				 uaddr ? (struct sockaddr *)&sa : NULL,
				 uaddr ? &salen : NULL);
	if (err >= 0 && uaddr && uaddrlen) {
		int n = min(ulen, salen);

		if (copy_to_user(uaddr, &sa, n) ||
		    copy_to_user(uaddrlen, &salen, sizeof(salen)))
			err = -EFAULT;
	}
	fs_file_put(file);
	return err;
}

u64 sys_shutdown(void)
{
	int fd = syscall_arg_int(0);
	int how = syscall_arg_int(1);
	struct fs_file *file;
	struct socket *sock;
	int err;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	if (!sock->ops->shutdown) {
		err = -ENOSYS;
	} else {
		err = sock->ops->shutdown(sock, how);
	}
	fs_file_put(file);
	return err;
}

u64 sys_listen(void)
{
	int fd = syscall_arg_int(0);
	int backlog = syscall_arg_int(1);
	struct fs_file *file;
	struct socket *sock;
	int err;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	if (!sock->ops->listen) {
		err = -EOPNOTSUPP;
	} else {
		err = sock->ops->listen(sock, backlog);
	}
	fs_file_put(file);
	return err;
}

static u64 sys_accept_common(unsigned int flags)
{
	int fd = syscall_arg_int(0);
	void *uaddr = syscall_arg_ptr(1);
	int *uaddrlen = syscall_arg_ptr(2);
	struct fs_file *file, *nfile;
	struct socket *sock, *newsock;
	int ulen = 0, salen;
	int err, nfd;

	if (uaddr && uaddrlen) {
		if (copy_from_user(&ulen, uaddrlen, sizeof(ulen)))
			return -EFAULT;
		if (ulen < 0)
			ulen = 0;
	}

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;
	if (!sock->ops->accept) {
		fs_file_put(file);
		return -EOPNOTSUPP;
	}

	err = sock->ops->accept(sock, &newsock);
	fs_file_put(file);
	if (err)
		return err;

	if (flags & SOCK_NONBLOCK)
		newsock->nonblock = true;

	nfile = fs_file_alloc_anon(&socket_file_ops, newsock);
	if (IS_ERR(nfile)) {
		sock_release(newsock);
		return PTR_ERR(nfile);
	}

	nfd = fdtable_alloc_fd(current_task()->fdtable, nfile);
	if (nfd < 0) {
		fs_file_put(nfile);
		return nfd;
	}

	if (uaddr && uaddrlen && newsock->ops->getname) {
		struct sockaddr_in sa;
		int n;

		salen = sizeof(sa);
		err = newsock->ops->getname(newsock, (struct sockaddr *)&sa,
					    &salen, true);
		if (!err) {
			n = min(ulen, salen);
			copy_to_user(uaddr, &sa, n);
			copy_to_user(uaddrlen, &salen, sizeof(salen));
		}
	}

	return nfd;
}

u64 sys_accept(void)
{
	return sys_accept_common(0);
}

u64 sys_accept4(void)
{
	return sys_accept_common((unsigned int)syscall_arg_int(3));
}

static u64 sys_getname(bool peer)
{
	int fd = syscall_arg_int(0);
	void *uaddr = syscall_arg_ptr(1);
	int *uaddrlen = syscall_arg_ptr(2);
	struct sockaddr_in sa;
	struct fs_file *file;
	struct socket *sock;
	int salen;
	int ulen;
	int err;

	if (!uaddr || !uaddrlen)
		return -EINVAL;
	if (copy_from_user(&ulen, uaddrlen, sizeof(ulen)))
		return -EFAULT;
	if (ulen < 0)
		ulen = 0;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	if (!sock->ops->getname) {
		fs_file_put(file);
		return -ENOSYS;
	}

	salen = ulen;
	err = sock->ops->getname(sock, (struct sockaddr *)&sa, &salen, peer);
	if (!err) {
		int n = min(ulen, salen);

		if (copy_to_user(uaddr, &sa, n) ||
		    copy_to_user(uaddrlen, &salen, sizeof(salen)))
			err = -EFAULT;
	}
	fs_file_put(file);
	return err;
}

u64 sys_getsockname(void)
{
	return sys_getname(false);
}

u64 sys_getpeername(void)
{
	return sys_getname(true);
}

u64 sys_setsockopt(void)
{
	int fd = syscall_arg_int(0);
	int level = syscall_arg_int(1);
	int optname = syscall_arg_int(2);
	struct fs_file *file;
	struct socket *sock;
	int err;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;
	fs_file_put(file);
	(void)sock;

	if (level == SOL_SOCKET &&
	    (optname == SO_REUSEADDR || optname == SO_RCVBUF ||
	     optname == SO_SNDBUF))
		return 0;

	return -ENOPROTOOPT;
}

u64 sys_getsockopt(void)
{
	int fd = syscall_arg_int(0);
	struct fs_file *file;
	struct socket *sock;
	int err;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;
	fs_file_put(file);
	(void)sock;

	return -ENOPROTOOPT;
}
