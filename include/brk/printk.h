#ifndef BRK_PRINTK_H
#define BRK_PRINTK_H

#include <brk/types.h>

void printk(char const *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(char const *fmt, va_list ap);

#endif
