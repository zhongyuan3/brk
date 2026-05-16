#include <brk/console.h>
#include <brk/errno.h>
#include <brk/kernel.h>
#include <brk/lock.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/time.h>
#include <brk/ktime.h>

static SPINLOCK_DEFINE(printk_lock);

static int display_write(struct display *dis, char const *buf, usize_t len,
			 usize_t *wlen)
{
	(void)dis;

	for (usize_t i = 0; i < len; ++i)
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

static void vprintk_locked(const char *fmt, va_list ap)
{
	struct display dis = {
		.write = display_write,
		.priv = NULL,
	};
	printf_core(&dis, fmt, ap);
}

static void printk_locked(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintk_locked(fmt, ap);
	va_end(ap);
}

void vprintk(char const *fmt, va_list ap)
{
	spinlock_acquire(&printk_lock);
	vprintk_locked(fmt, ap);
	spinlock_release(&printk_lock);
}

void klog(int level, const char *fmt, ...)
{
	va_list ap;
	struct timespec ts = { 0 };

	ktime_get_boot_ts(&ts);

	va_start(ap, fmt);
	spinlock_acquire(&printk_lock);
	printk_locked("<%d>[%9ld.%09ld] ", level, ts.tv_sec, ts.tv_nsec);
	vprintk_locked(fmt, ap);
	spinlock_release(&printk_lock);
	va_end(ap);
}
