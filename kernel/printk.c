#include <aosd/macros.h>
#include <aosd/printf.h>
#include <aosd/printk.h>
#include <aosd/sbi.h>

static int display_write(struct display *dis, char const *buf, size_t len,
			 size_t *wlen)
{
	for (size_t i = 0; i < len; ++i)
		sbi_console_putchar(buf[i]);

	if (wlen)
		*wlen = len;

	return 0;
}

void printk(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}

void vprintk(char const *fmt, va_list ap)
{
	struct display dis = {
		.write = display_write,
		.priv = NULL,
	};
	printf_core(&dis, fmt, ap);
}
