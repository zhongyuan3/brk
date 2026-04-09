#ifndef STDIO_H
#define STDIO_H

#include <brk/printk.h>

#define printf(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define fflush(stream) ((void)0)

#endif
