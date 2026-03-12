#include "ulib.h"

struct dprintf_display {
	int fd;
	char *buf;
	size_t buf_size;
	size_t wcnt;
};

static char dprintf_io_buf[1024];

static void dprintf_flush(struct dprintf_display *d)
{
	write(d->fd, d->buf, d->wcnt);
	d->wcnt = 0;
}

static int dprintf_write(struct display *dis, char const *buf, size_t len,
			 size_t *wlen)
{
	struct dprintf_display *d = dis->priv;
	size_t target = len;
	while (len > 0) {
		if (len + d->wcnt > d->buf_size)
			dprintf_flush(d);

		size_t avail = d->buf_size - d->wcnt;
		size_t act = len < avail ? len : avail;

		memcpy(d->buf + d->wcnt, buf, act);
		d->wcnt += act;

		len -= act;
		buf += act;
	}
	if (wlen)
		*wlen = target;
	return 0;
}

int dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	int ret;
	va_start(ap, fmt);
	ret = vdprintf(fd, fmt, ap);
	va_end(ap);
	return ret;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
	struct dprintf_display priv = {
		.fd = fd,
		.buf = dprintf_io_buf,
		.buf_size = sizeof(dprintf_io_buf),
		.wcnt = 0,
	};
	struct display dis = {
		.write = dprintf_write,
		.priv = &priv,
	};
	int ret = printf_core(&dis, fmt, ap);
	dprintf_flush(&priv);
	return ret;
}

void perror(const char *s)
{
	dprintf(STDERR_FILENO, "%s: %s\n", s, strerror(errno));
}
