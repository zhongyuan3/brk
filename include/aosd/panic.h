#ifndef AOSD_PANIC_H
#define AOSD_PANIC_H

#include <aosd/types.h>

void panic(char const *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));

extern volatile bool panicked;

#endif
