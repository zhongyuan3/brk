#ifndef BRK_PRINTK_H
#define BRK_PRINTK_H

#include <brk/compiler.h>
#include <brk/stdargs.h>
#include <brk/types.h>

#define KLOG_DEBUG 0
#define KLOG_INFO 1
#define KLOG_WARN 2
#define KLOG_ERROR 3

/* Level used by plain printk()/vprintk(). */
#define KLOG_DEFAULT KLOG_DEBUG

#ifndef KLOG_LEVEL
#define KLOG_LEVEL KLOG_DEBUG
#endif

void printk(char const *fmt, ...) __printf_format(1, 2);
void vprintk(char const *fmt, va_list ap);
void klog(int level, const char *fmt, ...) __printf_format(2, 3);

/* Drain the accumulated log ring buffer to the consoles. */
void printk_flush_pending(void);

/*
 * Panic helper: append a pre-formatted message to the log ring buffer and
 * flush it, tolerating the lock states found on the panic path. Falls back
 * to a raw console write if printk_lock cannot be taken.
 */
void printk_panic_text(const char *text, size_t len);

#if KLOG_DEBUG >= KLOG_LEVEL
#define klog_debug(fmt, ...) klog(KLOG_DEBUG, fmt, ##__VA_ARGS__)
#else
#define klog_debug(fmt, ...) ((void)0)
#endif

#if KLOG_INFO >= KLOG_LEVEL
#define klog_info(fmt, ...) klog(KLOG_INFO, fmt, ##__VA_ARGS__)
#else
#define klog_info(fmt, ...) ((void)0)
#endif

#if KLOG_WARN >= KLOG_LEVEL
#define klog_warn(fmt, ...) klog(KLOG_WARN, fmt, ##__VA_ARGS__)
#else
#define klog_warn(fmt, ...) ((void)0)
#endif

#if KLOG_ERROR >= KLOG_LEVEL
#define klog_error(fmt, ...) klog(KLOG_ERROR, fmt, ##__VA_ARGS__)
#else
#define klog_error(fmt, ...) ((void)0)
#endif

#endif