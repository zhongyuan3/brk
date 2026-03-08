#include "ulib.h"

int dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	int ret;
	va_start(ap, fmt);
	ret = vdprintf(fd, fmt, ap);
	va_end(ap);
	return ret;
}

static int dis_write(struct display *dis, char const *buf, size_t len,
		     size_t *wlen)
{
	int *fd = dis->priv;
	ssize_t n = write(*fd, buf, len);
	if (n < 0)
		return n;
	if (wlen)
		*wlen = n;
	return 0;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
	struct display dis = {
		.write = dis_write,
		.priv = &fd,
	};
	return printf_core(&dis, fmt, ap);
}

void perror(const char *s)
{
	dprintf(STDERR_FILENO, "%s: %s\n", s, strerror(errno));
}
