#ifndef STDIO_H
#define STDIO_H

#include <aosd/printk.h>

#define printf(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define fflush(stream) ((void)0)

#endif
