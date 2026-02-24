#ifndef AOSD_CONSOLE_H
#define AOSD_CONSOLE_H

#include <aosd/types.h>

#define CONSOLE_BUF_SIZE 1024

void console_init(void);
void console_putc(int c);
int console_write(const char *buf, size_t n, size_t *written);
int console_read(char *buf, size_t n, size_t *read);
void console_intr(int c);

#endif
