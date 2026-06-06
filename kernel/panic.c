#include <arch/irqflags.h>
#include <brk/assert.h>
#include <brk/earlycon.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/string.h>

static volatile int in_panic;
static char panic_buf[512];

void panic(char const *fmt, ...)
{
	va_list ap;

	intr_off();

	/*
	 * Never route panic through printk()/printk_lock: a panic may fire
	 * while the current CPU already holds printk_lock, in which case
	 * re-entering spinlock_acquire() would itself panic and recurse
	 * forever (overflowing the stack and corrupting memory). Render the
	 * message into a local buffer and push it straight to the SBI
	 * console instead.
	 */
	if (__sync_lock_test_and_set(&in_panic, 1)) {
		for (;;)
			;
	}

	memset(panic_buf, 0, sizeof(panic_buf));

	va_start(ap, fmt);
	vsnprintf(panic_buf, sizeof(panic_buf) - 1, fmt, ap);
	va_end(ap);

	earlycon_putstr("PANIC: ");
	earlycon_putstr(panic_buf);

	for (;;)
		;
}

void __assert_fail(char const *file, int line, char const *expr)
{
	panic("Assertion failed: %s:%d: %s\n", file, line, expr);
}
