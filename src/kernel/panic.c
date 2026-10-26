#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/string.h>

static volatile int in_panic;
static char panic_buf[512];

void panic(char const *fmt, ...)
{
	va_list ap;
	size_t len;

	local_irq_disable();

	/*
	 * Never route panic through printk() itself: a panic may fire while
	 * the current CPU already holds printk_lock, in which case re-entering
	 * spinlock_acquire() would recurse forever. Render the message into a
	 * local buffer and hand it to printk_panic_text(), which tolerates the
	 * lock states found on the panic path.
	 */
	if (__sync_lock_test_and_set(&in_panic, 1)) {
		for (;;)
			;
	}

	memset(panic_buf, 0, sizeof(panic_buf));
	strlcpy(panic_buf, "PANIC: ", sizeof(panic_buf));

	va_start(ap, fmt);
	vsnprintf(panic_buf + 7, sizeof(panic_buf) - 7 - 1, fmt, ap);
	va_end(ap);

	len = strnlen(panic_buf, sizeof(panic_buf));

	printk_panic_text(panic_buf, len);

	for (;;)
		;
}

void __assert_fail(char const *file, int line, char const *expr)
{
	panic("Assertion failed: %s:%d: %s\n", file, line, expr);
}