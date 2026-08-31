#include <brk/base/kernel.h>
#include <brk/lib/printf.h>
#include <brk/lib/string.h>
#include <brk/lock/spinlock.h>
#include <brk/printk/console.h>
#include <brk/printk/logbuf.h>
#include <brk/printk/printk.h>
#include <brk/time/ktime.h>
#include <uapi/brk/errno.h>

static SPINLOCK_DEFINE(printk_lock);
static char printk_buf[1024];

void printk(char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}

/*
 * Format the message, append it to the log ring buffer and, if any console
 * is active, drain the buffer so the record is printed. Caller must hold
 * printk_lock.
 */
static void vprintk_locked(int level, const char *fmt, va_list ap)
{
	int n = vsnprintf(printk_buf, sizeof(printk_buf), fmt, ap);

	if (n > 0)
		log_store(level, printk_buf,
			  strnlen(printk_buf, sizeof(printk_buf)));
	if (console_has_active())
		log_flush();
}

void vprintk(char const *fmt, va_list ap)
{
	spinlock_acquire(&printk_lock);
	vprintk_locked(KLOG_DEFAULT, fmt, ap);
	spinlock_release(&printk_lock);
}

void klog(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	spinlock_acquire(&printk_lock);
	vprintk_locked(level, fmt, ap);
	spinlock_release(&printk_lock);
	va_end(ap);
}

void printk_flush_pending(void)
{
	spinlock_acquire(&printk_lock);
	log_flush();
	spinlock_release(&printk_lock);
}

void printk_panic_text(const char *text, size_t len)
{
	if (spinlock_holding(&printk_lock)) {
		/* Panic fired inside printk(): we already own the lock. */
		log_store(KLOG_ERROR, text, len);
		log_flush();
	} else if (spinlock_tryacquire(&printk_lock)) {
		log_store(KLOG_ERROR, text, len);
		log_flush();
		spinlock_release(&printk_lock);
	} else {
		/* Another CPU holds the lock; best-effort raw output. */
		console_write_record_direct(KLOG_ERROR, ktime_get_boot_ns(),
					    text, len);
	}
}