#ifndef AOSD_CONSOLE_H
#define AOSD_CONSOLE_H

#include <aosd/types.h>

struct console {
	struct console *next;
	int (*write)(struct console *con, char const *buf, size_t n,
		     size_t *written);
	int (*read)(struct console *con, char *buf, size_t n, size_t *read);
};

void console_register(struct console *con);
int console_unregister(struct console *con);

#endif
