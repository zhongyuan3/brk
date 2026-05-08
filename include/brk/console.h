#ifndef BRK_CONSOLE_H
#define BRK_CONSOLE_H

#include <brk/types.h>

/* Must match TTY_RX_BUF_SIZE in tty.h (boot console uses tty_boot). */
#define CONSOLE_BUF_SIZE 1024

void console_init(void);
void console_putc(int c);

#endif
