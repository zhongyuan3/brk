#ifndef AOSD_PRINTF_H
#define AOSD_PRINTF_H

#include <aosd/types.h>

struct display {
	int (*write)(struct display *dis, char const *buf, size_t len,
		     size_t *wlen);
	void *priv;
};

int printf_core(struct display *dis, char const *format, va_list ap);

#endif
