#ifndef AOSD_TYPES_H
#define AOSD_TYPES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct list_head {
	struct list_head *prev, *next;
};

#endif
