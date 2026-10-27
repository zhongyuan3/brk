#ifndef BRK_PRINTK_H
#define BRK_PRINTK_H

#include <brk/compiler.h>
#include <brk/stdargs.h>
#include <brk/types.h>

#define KERN_EMERG 0
#define KERN_ALERT 1
#define KERN_CRIT 2
#define KERN_ERR 3
#define KERN_WARN 4
#define KERN_NOTICE 5
#define KERN_INFO 6
#define KERN_DEBUG 7

#define KERN_MAX KERN_DEBUG

#define KERN_DEFAULT KERN_WARN

void printk(char const *fmt, ...) __printf_format(1, 2);
void vprintk(char const *fmt, va_list ap);
void _printk(int level, const char *fmt, ...) __printf_format(2, 3);

/* Drain the accumulated log ring buffer to the consoles. */
void printk_flush_pending(void);

/*
 * Consume the oldest record from the printk ring buffer into @buf.
 * @buf is NUL-terminated when a record is returned.
 *
 * Return the text length, 0 if the buffer is empty, or -ENOSPC if the
 * record did not fit in @size (the record is consumed either way).
 */
int printk_read_record(char *buf, size_t size, u8 *level, u64 *ts_nsec);

/*
 * Panic helper: append a pre-formatted message to the log ring buffer and
 * flush it, tolerating the lock states found on the panic path. Falls back
 * to a raw console write if printk_lock cannot be taken.
 */
void printk_panic_text(const char *text, size_t len);

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define pr_emerg(fmt, ...) _printk(KERN_EMERG, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...) _printk(KERN_ALERT, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...) _printk(KERN_CRIT, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...) _printk(KERN_ERR, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) _printk(KERN_WARN, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...) _printk(KERN_NOTICE, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) _printk(KERN_INFO, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) _printk(KERN_DEBUG, pr_fmt(fmt), ##__VA_ARGS__)

#endif
