#include <brk/console.h>
#include <brk/dev.h>
#include <brk/tty.h>
#include <brk/uart.h>

void console_init(void)
{
	tty_boot_init(uart_tty_port());
}

int console_register_dev(void)
{
	return tty_chrdev_register(tty_boot(), DEV_CONSOLE0);
}
