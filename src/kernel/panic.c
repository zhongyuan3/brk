#include <assert.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <string.h>

#define PANIC_BUF_SIZE 512
static char panic_buf[PANIC_BUF_SIZE];

void panic(char const *fmt, ...)
{
	va_list ap;
	size_t len;

	local_irq_disable();

	memset(panic_buf, 0, PANIC_BUF_SIZE);
	strlcpy(panic_buf, "PANIC: ", PANIC_BUF_SIZE);

	va_start(ap, fmt);
	vsnprintf(panic_buf + 7, PANIC_BUF_SIZE - 7 - 1, fmt, ap);
	va_end(ap);

	len = strnlen(panic_buf, PANIC_BUF_SIZE);

	printk_panic_text(panic_buf, len);

	for (;;)
		;
}

void __assert_fail(char const *file, int line, char const *expr)
{
	panic("assertion failed: %s:%d: %s\n", file, line, expr);
}
