#ifndef AOSD_PRINTK_H
#define AOSD_PRINTK_H

#include <aosd/types.h>

void printk(char const *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(char const *fmt, va_list ap);

#define log_debug(fmt, ...) printk("[DEBUG] " fmt, ##__VA_ARGS__)
#define log_info(fmt, ...) printk("[INFO ] " fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) printk("[WARN ] " fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) printk("[ERROR] " fmt, ##__VA_ARGS__)

#endif
