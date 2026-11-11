#ifndef BRK_CONSOLE_H
#define BRK_CONSOLE_H

#include <brk/list.h>
#include <brk/printk_log.h>
#include <brk/types.h>

#define CONSOLE_FLAG_BOOT (1u << 0)

struct console {
	void *client_data;
	struct list_head list;
	int (*put_char)(struct console *console, int c);
	int (*put_char_atomic)(struct console *console, int c);
	bool active;
	unsigned int flags;
	u8 log_level;
	struct printk_log_reader reader;
};

int console_set_log_level(struct console *console, u8 level);

/*
 * @log_level filters the pending records that are flushed during
 * registration, so pass the final level here instead of changing it after
 * console_register() returns.
 */
void console_register(struct console *console, u8 log_level);

/* Change the log level of consoles marked CONSOLE_FLAG_BOOT. */
void console_set_boot_consoles_level(u8 level);

/* Best-effort panic output paths; they do not take console_lock. */
void console_flush_on_panic(void);
void console_write_record_panic(u8 level, u64 ts_nsec, const char *text,
				size_t len);
void console_unregister(struct console *console);
void console_deactivate(struct console *console);

bool console_has_active(void);

/* Start the asynchronous console log consumer thread. */
int console_log_start(void);

/* Output one formatted record (<level>[sec.nsec] text) to all consoles. */
void console_write_record(u8 level, u64 ts_nsec, const char *text, size_t len);

#endif
