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

void console_register(struct console *console);
void console_unregister(struct console *console);

bool console_has_active(void);

/* Output one formatted record (<level>[sec.nsec] text) to all consoles. */
void console_write_record(uint8_t level, uint64_t ts_nsec, const char *text,
			  size_t len);

/*
 * Best-effort, lock-free variant used on the panic path when the printk
 * lock cannot be taken; it may race with concurrent console output.
 */
void console_write_record_direct(uint8_t level, uint64_t ts_nsec,
				 const char *text, size_t len);

#endif