#include <brk/assert.h>
#include <brk/cpu.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/printk.h>
#include <brk/string.h>

#define PANIC_BUF_SIZE 512

static int panic_owner;

void panic(char const *fmt, ...)
{
	char buf[PANIC_BUF_SIZE];
	va_list ap;
	size_t len;
	long cpu = (long)this_cpuid_safe();
	int expected = 0;

	local_irq_disable();

	if (!__atomic_compare_exchange_n(&panic_owner, &expected, 1, false,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		int n = snprintf(
			buf, sizeof(buf),
			"PANIC: CPU %ld: another CPU is already panicking\n",
			cpu);

		if (n > 0) {
			if ((size_t)n >= sizeof(buf))
				n = sizeof(buf) - 1;
			printk_panic_text(buf, (size_t)n);
		}

		for (;;)
			__asm__ volatile("wfi");
	}

	memset(buf, 0, sizeof(buf));
	strlcpy(buf, "PANIC: ", sizeof(buf));

	va_start(ap, fmt);
	vsnprintf(buf + 7, sizeof(buf) - 7 - 1, fmt, ap);
	va_end(ap);

	len = strnlen(buf, sizeof(buf));

	printk_panic_text(buf, len);

	for (;;)
		__asm__ volatile("wfi");
}

void __assert_fail(char const *file, int line, char const *expr)
{
	panic("assertion failed: %s:%d: %s\n", file, line, expr);
}
