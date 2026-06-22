#ifndef BRK_PRINTK_H
#define BRK_PRINTK_H

#include <brk/compiler.h>
#include <brk/types.h>

#define KLOG_DEBUG 0
#define KLOG_INFO 1
#define KLOG_WARN 2
#define KLOG_ERROR 3

#ifndef KLOG_LEVEL
#define KLOG_LEVEL KLOG_INFO
#endif

void printk(char const *fmt, ...) __printf_format(1, 2);
void vprintk(char const *fmt, va_list ap);
void klog(int level, const char *fmt, ...) __printf_format(2, 3);

#if KLOG_DEBUG >= KLOG_LEVEL
#define klog_debug(fmt, ...) klog(KLOG_DEBUG, fmt, ##__VA_ARGS__);
#else
#define klog_debug(fmt, ...) ((void)0)
#endif

#if KLOG_INFO >= KLOG_LEVEL
#define klog_info(fmt, ...) klog(KLOG_INFO, fmt, ##__VA_ARGS__);
#else
#define klog_info(fmt, ...) ((void)0)
#endif

#if KLOG_WARN >= KLOG_LEVEL
#define klog_warn(fmt, ...) klog(KLOG_WARN, fmt, ##__VA_ARGS__);
#else
#define klog_warn(fmt, ...) ((void)0)
#endif

#if KLOG_ERROR >= KLOG_LEVEL
#define klog_error(fmt, ...) klog(KLOG_ERROR, fmt, ##__VA_ARGS__);
#else
#define klog_error(fmt, ...) ((void)0)
#endif

#endif
