#include <brk/console.h>
#include <brk/errno.h>
#include <brk/kernel.h>
#include <brk/lock.h>
#include <brk/printf.h>
#include <brk/printk.h>

static SPINLOCK_DEFINE(printk_lock);

static int display_write(struct display *dis, char const *buf, size_t len,
			 size_t *wlen)
{
	(void)dis;

	for (size_t i = 0; i < len; ++i)
		console_putc(buf[i]);

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
	spinlock_acquire(&printk_lock);
	printf_core(&dis, fmt, ap);
	spinlock_release(&printk_lock);
}
