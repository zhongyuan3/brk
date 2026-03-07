#include <aosd/assert.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>

volatile bool panicked;

void panic(char const *fmt, ...)
{
	va_list ap;

	intr_off();

	printk("PANIC: ");

	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
	panicked = true;

	for (;;)
		;
}

void assert_fail(char const *file, int line, char const *expr)
{
	intr_off();
	printk("Assertion failed: %s:%d: %s\n", file, line, expr);
	for (;;)
		;
}
