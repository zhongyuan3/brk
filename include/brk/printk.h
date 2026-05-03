#ifndef BRK_PRINTK_H
#define BRK_PRINTK_H

#include <brk/compiler.h>
#include <brk/types.h>

#define LOG_LEVEL_TRACE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_WARN 3
#define LOG_LEVEL_ERROR 4

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

#ifndef LOG_COLOR_ENABLE
#define LOG_COLOR_ENABLE 1
#endif

#if LOG_COLOR_ENABLE
#define LOG_COLOR_TRACE "\033[90m"
#define LOG_COLOR_DEBUG "\033[94m"
#define LOG_COLOR_INFO "\033[92m"
#define LOG_COLOR_WARN "\033[93m"
#define LOG_COLOR_ERROR "\033[91m"
#define LOG_COLOR_RESET "\033[0m"
#else
#define LOG_COLOR_TRACE ""
#define LOG_COLOR_DEBUG ""
#define LOG_COLOR_INFO ""
#define LOG_COLOR_WARN ""
#define LOG_COLOR_ERROR ""
#define LOG_COLOR_RESET ""
#endif

void printk(char const *fmt, ...) __printf_format(1, 2);
void vprintk(char const *fmt, va_list ap);

#if LOG_LEVEL_TRACE >= LOG_LEVEL
#define log_trace(fmt, ...) \
	printk(LOG_COLOR_TRACE "[TRACE]" LOG_COLOR_RESET " " fmt, ##__VA_ARGS__)
#else
#define log_trace(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL_DEBUG >= LOG_LEVEL
#define log_debug(fmt, ...) \
	printk(LOG_COLOR_DEBUG "[DEBUG]" LOG_COLOR_RESET " " fmt, ##__VA_ARGS__)
#else
#define log_debug(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL_INFO >= LOG_LEVEL
#define log_info(fmt, ...) \
	printk(LOG_COLOR_INFO "[INFO ]" LOG_COLOR_RESET " " fmt, ##__VA_ARGS__)
#else
#define log_info(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL_WARN >= LOG_LEVEL
#define log_warn(fmt, ...) \
	printk(LOG_COLOR_WARN "[WARN ]" LOG_COLOR_RESET " " fmt, ##__VA_ARGS__)
#else
#define log_warn(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL_ERROR >= LOG_LEVEL
#define log_error(fmt, ...) \
	printk(LOG_COLOR_ERROR "[ERROR]" LOG_COLOR_RESET " " fmt, ##__VA_ARGS__)
#else
#define log_error(fmt, ...) ((void)0)
#endif

#endif
