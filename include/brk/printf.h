#ifndef BRK_PRINTF_H
#define BRK_PRINTF_H

#include <brk/types.h>

struct display {
	int (*write)(struct display *dis, char const *buf, size_t len,
		     size_t *wlen);
	void *priv;
};

int printf_core(struct display *dis, char const *format, va_list ap);

int snprintf(char *buf, size_t size, char const *format, ...)
	__attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, char const *format, va_list ap);

#endif
