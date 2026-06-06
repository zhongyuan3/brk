#include <arch/sbi.h>
#include <brk/console.h>

static int early_console_put_char(struct console *console, int c)
{
	(void)console;
	sbi_console_putchar(c);
	return 0;
}

struct console early_console = {
	.put_char = early_console_put_char,
	.active = true,
};

void earlycon_putchar(int c)
{
	sbi_console_putchar(c);
}

void earlycon_putstr(const char *s)
{
	sbi_console_putstr(s);
}
