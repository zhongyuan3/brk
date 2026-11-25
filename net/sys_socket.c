#include <brk/error.h>
#include <brk/fdtable.h>
#include <brk/fs.h>
#include <brk/min_max.h>
#include <brk/socket.h>
#include <brk/spinlock.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <brk/types.h>
#include <brk/uaccess.h>
#include <uapi/errno.h>
#include <uapi/socket.h>

static int socket_from_fd(int fd, struct file **filep, struct socket **sockp)
{
	struct file *file;

	if (fd < 0)
		return -EBADF;

	file = fdtable_get_file(&current_process()->fdtable, fd);
	if (IS_ERR(file))
		return PTR_ERR(file);

	if (file->ops != &socket_file_ops) {
		file_put(file);
		return -ENOTSOCK;
	}

	*filep = file;
	*sockp = file->private_data;
	return 0;
}

long sys_create_socket(const struct syscall_ctx *ctx)
{
	int family = syscall_arg_int(ctx, 0);
	int type = syscall_arg_int(ctx, 1);
	int protocol = syscall_arg_int(ctx, 2);
	bool nonblock = (type & SOCK_NONBLOCK) != 0;
	struct file *file;
	struct socket *sock;
	int fd;
	int err;

	type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

	err = sock_create(family, type, protocol, &sock);
	if (err)
		return err;

	sock->nonblock = nonblock;

	file = file_alloc_anon(&socket_file_ops, sock);
	if (IS_ERR(file)) {
		sock_release(sock);
		return PTR_ERR(file);
	}

	fd = fdtable_alloc_fd(&current_process()->fdtable, file);
	if (fd < 0) {
		file_put(file);
		return fd;
	}
	return fd;
}

long sys_bind(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	const void *uaddr = syscall_arg_ptr(ctx, 1);
	int addrlen = syscall_arg_int(ctx, 2);
	struct __brk_sockaddr_in sa;
	struct file *file;
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

	err = sock->ops->bind(sock, (const struct __brk_sockaddr *)&sa,
			      addrlen);
	file_put(file);
	return err;
}

long sys_connect(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	const void *uaddr = syscall_arg_ptr(ctx, 1);
	int addrlen = syscall_arg_int(ctx, 2);
	struct __brk_sockaddr_in sa;
	struct file *file;
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

	err = sock->ops->connect(sock, (const struct __brk_sockaddr *)&sa,
				 addrlen);
	file_put(file);
	return err;
}

long sys_send_to(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	const void *buf = syscall_arg_ptr(ctx, 1);
	size_t len = syscall_arg_raw(ctx, 2);
	unsigned int flags = syscall_arg_raw(ctx, 3);
	const void *uaddr = syscall_arg_ptr(ctx, 4);
	int addrlen = syscall_arg_int(ctx, 5);
	struct __brk_sockaddr_in sa;
	struct file *file;
	struct socket *sock;
	int err;

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
				 uaddr ? (const struct __brk_sockaddr *)&sa :
					 NULL,
				 uaddr ? (int)sizeof(sa) : 0, (int)flags);
	file_put(file);
	return err;
}

long sys_recv_from(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	void *buf = syscall_arg_ptr(ctx, 1);
	size_t len = syscall_arg_raw(ctx, 2);
	unsigned int flags = syscall_arg_raw(ctx, 3);
	void *uaddr = syscall_arg_ptr(ctx, 4);
	int *uaddrlen = syscall_arg_ptr(ctx, 5);
	struct __brk_sockaddr_in sa;
	struct file *file;
	struct socket *sock;
	int salen = sizeof(sa);
	int ulen = 0;
	int err;

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
				 uaddr ? (struct __brk_sockaddr *)&sa : NULL,
				 uaddr ? &salen : NULL, (int)flags);
	if (err >= 0 && uaddr && uaddrlen) {
		int n = min(ulen, salen);

		if (copy_to_user(uaddr, &sa, n) ||
		    copy_to_user(uaddrlen, &salen, sizeof(salen)))
			err = -EFAULT;
	}
	file_put(file);
	return err;
}

long sys_shutdown(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	int how = syscall_arg_int(ctx, 1);
	struct file *file;
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
	file_put(file);
	return err;
}

long sys_listen(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	int backlog = syscall_arg_int(ctx, 1);
	struct file *file;
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
	file_put(file);
	return err;
}

static u64 sys_accept_common(const struct syscall_ctx *ctx, unsigned int flags)
{
	int fd = syscall_arg_int(ctx, 0);
	void *uaddr = syscall_arg_ptr(ctx, 1);
	int *uaddrlen = syscall_arg_ptr(ctx, 2);
	struct file *file, *nfile;
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
		file_put(file);
		return -EOPNOTSUPP;
	}

	err = sock->ops->accept(sock, &newsock);
	file_put(file);
	if (err)
		return err;

	if (flags & SOCK_NONBLOCK)
		newsock->nonblock = true;

	nfile = file_alloc_anon(&socket_file_ops, newsock);
	if (IS_ERR(nfile)) {
		sock_release(newsock);
		return PTR_ERR(nfile);
	}

	nfd = fdtable_alloc_fd(&current_process()->fdtable, nfile);
	if (nfd < 0) {
		file_put(nfile);
		return nfd;
	}

	if (uaddr && uaddrlen && newsock->ops->getname) {
		struct __brk_sockaddr_in sa;
		int n;

		salen = sizeof(sa);
		err = newsock->ops->getname(
			newsock, (struct __brk_sockaddr *)&sa, &salen, true);
		if (!err) {
			n = min(ulen, salen);
			copy_to_user(uaddr, &sa, n);
			copy_to_user(uaddrlen, &salen, sizeof(salen));
		}
	}

	return nfd;
}

long sys_accept(const struct syscall_ctx *ctx)
{
	return sys_accept_common(ctx, (unsigned int)syscall_arg_int(ctx, 3));
}

static u64 sys_getname(const struct syscall_ctx *ctx, bool peer)
{
	int fd = syscall_arg_int(ctx, 0);
	void *uaddr = syscall_arg_ptr(ctx, 1);
	int *uaddrlen = syscall_arg_ptr(ctx, 2);
	struct __brk_sockaddr_in sa;
	struct file *file;
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
		file_put(file);
		return -ENOSYS;
	}

	salen = ulen;
	err = sock->ops->getname(sock, (struct __brk_sockaddr *)&sa, &salen,
				 peer);
	if (!err) {
		int n = min(ulen, salen);

		if (copy_to_user(uaddr, &sa, n) ||
		    copy_to_user(uaddrlen, &salen, sizeof(salen)))
			err = -EFAULT;
	}
	file_put(file);
	return err;
}

long sys_get_sock_addr(const struct syscall_ctx *ctx)
{
	return sys_getname(ctx, false);
}

long sys_get_peer_addr(const struct syscall_ctx *ctx)
{
	return sys_getname(ctx, true);
}

long sys_set_sock_opt(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	int level = syscall_arg_int(ctx, 1);
	int optname = syscall_arg_int(ctx, 2);
	const void *optval = syscall_arg_ptr(ctx, 3);
	int optlen = syscall_arg_int(ctx, 4);
	struct file *file;
	struct socket *sock;
	int value;
	int err;

	if (optlen < (int)sizeof(value) || !optval)
		return -EINVAL;
	if (copy_from_user(&value, optval, sizeof(value)))
		return -EFAULT;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	err = 0;
	if (level == SOL_SOCKET) {
		switch (optname) {
		case SO_REUSEADDR:
			if (sock->sk)
				sock->sk->reuseaddr = value != 0;
			break;
		case SO_BROADCAST:
			if (sock->sk)
				sock->sk->broadcast = value != 0;
			break;
		case SO_RCVBUF:
			if (sock->sk)
				sock->sk->rcvbuf = value < 1024 ? 1024 : value;
			break;
		case SO_SNDBUF:
			if (sock->sk)
				sock->sk->sndbuf = value < 1024 ? 1024 : value;
			break;
		default:
			err = -ENOPROTOOPT;
			break;
		}
	} else {
		err = -ENOPROTOOPT;
	}
	file_put(file);
	return err;
}

long sys_get_sock_opt(const struct syscall_ctx *ctx)
{
	int fd = syscall_arg_int(ctx, 0);
	int level = syscall_arg_int(ctx, 1);
	int optname = syscall_arg_int(ctx, 2);
	void *optval = syscall_arg_ptr(ctx, 3);
	int *optlen = syscall_arg_ptr(ctx, 4);
	struct file *file;
	struct socket *sock;
	irqstate_t flags;
	int value;
	int len;
	int err;

	if (!optval || !optlen)
		return -EINVAL;
	if (copy_from_user(&len, optlen, sizeof(len)))
		return -EFAULT;
	if (len < 0)
		len = 0;

	err = socket_from_fd(fd, &file, &sock);
	if (err)
		return err;

	if (level != SOL_SOCKET) {
		file_put(file);
		return -ENOPROTOOPT;
	}

	switch (optname) {
	case SO_ERROR:
		if (sock->sk) {
			flags = spin_lock_irqsave(&sock->sk->lock);
			value = sock->sk->err;
			sock->sk->err = 0;
			spin_unlock_irqrestore(&sock->sk->lock, flags);
		} else {
			value = 0;
		}
		break;
	case SO_TYPE:
		value = sock->type;
		break;
	case SO_BROADCAST:
		value = sock->sk ? sock->sk->broadcast : 0;
		break;
	case SO_RCVBUF:
		value = sock->sk ? (int)sock->sk->rcvbuf : 0;
		break;
	case SO_SNDBUF:
		value = sock->sk ? (int)sock->sk->sndbuf : 0;
		break;
	default:
		file_put(file);
		return -ENOPROTOOPT;
	}

	if (len > (int)sizeof(value))
		len = (int)sizeof(value);
	if (copy_to_user(optval, &value, len) ||
	    copy_to_user(optlen, &len, sizeof(len)))
		err = -EFAULT;
	file_put(file);
	return err;
}
