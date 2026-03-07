#ifndef AOSD_TYPES_H
#define AOSD_TYPES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct list_head {
	struct list_head *prev, *next;
};

typedef uint32_t dev_t;
typedef unsigned int mode_t;
typedef unsigned int fmode_t;
typedef long off_t;
typedef long pid_t;
typedef long clock_t;
typedef long suseconds_t;

#endif
