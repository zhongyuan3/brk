#ifndef BRK_PRINTK_H
#define BRK_PRINTK_H

#include <brk/compiler.h>
#include <brk/stdargs.h>
#include <brk/types.h>

#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DEBUG 3

#define LOG_LEVEL_DEFAULT LOG_LEVEL_WARN

void printk(char const *fmt, ...) __printf_format(1, 2);
void vprintk(char const *fmt, va_list ap);
void _printk(int level, const char *fmt, ...) __printf_format(2, 3);

/* Drain the accumulated log ring buffer to the consoles. */
void printk_flush_pending(void);

/*
 * Panic helper: append a pre-formatted message to the log ring buffer and
 * flush it, tolerating the lock states found on the panic path. Falls back
 * to a raw console write if printk_lock cannot be taken.
 */
void printk_panic_text(const char *text, size_t len);

#define pr_debug(fmt, ...) _printk(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) _printk(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) _printk(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define pr_error(fmt, ...) _printk(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#endif
