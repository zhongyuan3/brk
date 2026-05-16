#ifndef BRK_CONSOLE_H
#define BRK_CONSOLE_H

/* Boot UART console: TTY setup (early) and /dev/console registration (dev_init). */
void console_init(void);
int console_register_dev(void);

#endif
