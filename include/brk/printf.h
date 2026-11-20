#ifndef BRK_PRINTF_H
#define BRK_PRINTF_H

#include <brk/compiler_attr.h>
#include <stdarg.h>
#include <stddef.h>

int snprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
