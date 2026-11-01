#ifndef BRK_PRINTF_H
#define BRK_PRINTF_H

#include <brk/types.h>
#include <stdarg.h>

int snprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
