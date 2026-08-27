#ifndef BRK_PRINTF_H
#define BRK_PRINTF_H

#include <brk/base/compiler.h>
#include <brk/base/types.h>

int snprintf(char *buf, size_t size, const char *fmt, ...)
	__printf_format(3, 4);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
