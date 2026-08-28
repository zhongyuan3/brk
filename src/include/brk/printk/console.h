#ifndef BRK_CONSOLE_H
#define BRK_CONSOLE_H

#include <brk/base/list.h>
#include <brk/base/types.h>

struct console {
	void *client_data;
	struct list_head list;
	int (*put_char)(struct console *console, int c);
	bool active;
};

extern struct console early_console;

void console_register(struct console *console);
void console_unregister(struct console *console);

void console_write_all(const char *s, size_t n);

#endif
