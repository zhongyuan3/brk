#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/net/socket.h>
#include <brk/net/tcp.h>
#include <brk/net/udp.h>
#include <brk/string.h>
#include <brk/types.h>
#include <uapi/errno.h>
#include <uapi/socket.h>

int sock_create(int family, int type, int protocol, struct socket **res)
{
	struct socket *sock;
	int err;

	if (!res)
		return -EINVAL;
	if (family != AF_INET)
		return -EAFNOSUPPORT;

	sock = kzalloc(sizeof(*sock));
	if (!sock)
		return -ENOMEM;

	sock->type = type;

	switch (type) {
	case SOCK_DGRAM:
		err = udp_sock_create(sock, protocol);
		break;
	case SOCK_STREAM:
		err = tcp_sock_create(sock, protocol);
		break;
	default:
		err = -ESOCKTNOSUPPORT;
		break;
	}

	if (err) {
		kfree(sock);
		return err;
	}

	*res = sock;
	return 0;
}

void sock_release(struct socket *sock)
{
	if (!sock)
		return;

	if (sock->ops && sock->ops->release)
		sock->ops->release(sock);

	kfree(sock);
}

static ssize_t socket_read(struct fs_file *file, char *buf, size_t size,
			   loff_t *pos)
{
	struct socket *sock = file->private_data;

	(void)pos;

	if (!sock || !sock->ops || !sock->ops->recvmsg)
		return -ENOTSOCK;

	return sock->ops->recvmsg(sock, buf, size, NULL, NULL);
}

static ssize_t socket_write(struct fs_file *file, const char *buf, size_t size,
			    loff_t *pos)
{
	struct socket *sock = file->private_data;

	(void)pos;

	if (!sock || !sock->ops || !sock->ops->sendmsg)
		return -ENOTSOCK;

	return sock->ops->sendmsg(sock, buf, size, NULL, 0);
}

static int socket_file_release(struct fs_inode *inode, struct fs_file *file)
{
	(void)inode;

	if (file->private_data) {
		sock_release(file->private_data);
		file->private_data = NULL;
	}
	return 0;
}

static loff_t socket_llseek(struct fs_file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -ESPIPE;
}

static long socket_ioctl(struct fs_file *file, unsigned int cmd,
			 unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

const struct fs_file_ops socket_file_ops = {
	.release = socket_file_release,
	.read = socket_read,
	.write = socket_write,
	.llseek = socket_llseek,
	.ioctl = socket_ioctl,
};
