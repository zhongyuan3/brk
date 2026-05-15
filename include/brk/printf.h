#ifndef BRK_PRINTF_H
#define BRK_PRINTF_H

#include <brk/compiler.h>
#include <brk/types.h>

struct display {
	int (*write)(struct display *dis, char const *buf, usize_t len,
		     usize_t *wlen);
	void *priv;
};

int printf_core(struct display *dis, char const *format, va_list ap);

int snprintf(char *buf, usize_t size, char const *format, ...)
	__printf_format(3, 4);
int vsnprintf(char *buf, usize_t size, char const *format, va_list ap);

#endif
