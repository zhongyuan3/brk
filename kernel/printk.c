#include <brk/console.h>
#include <brk/errno.h>
#include <brk/kernel.h>
#include <brk/ktime.h>
#include <brk/lock.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/time.h>
#include <brk/tty.h>

static SPINLOCK_DEFINE(printk_lock);
static char printk_buf[1024];

void printk(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}

static void vprintk_locked(const char *fmt, va_list ap)
{
	int n = vsnprintf(printk_buf, sizeof(printk_buf), fmt, ap);
	if (n > 0)
		console_write_all(printk_buf, n);
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
