#include <brk/console.h>
#include <brk/tty.h>
#include <brk/uart.h>

#define BACKSPACE 0x100

void console_init(void)
{
	tty_boot_init();
}

void console_putc(int c)
{
	if (c == BACKSPACE) {
		uart_putc('\b');
		uart_putc(' ');
		uart_putc('\b');
	} else {
		uart_putc(c);
	}
}
