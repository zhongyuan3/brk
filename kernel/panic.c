#include <aosd/assert.h>
#include <aosd/panic.h>
#include <aosd/printk.h>
#include <aosd/riscv.h>

void panic(char const *fmt, ...)
{
	va_list ap;

	intr_off();

	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);

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
