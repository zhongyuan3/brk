#ifndef AOSD_PANIC_H
#define AOSD_PANIC_H

void panic(char const *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));

#endif
