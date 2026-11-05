#ifndef BRK_PANIC_H
#define BRK_PANIC_H

#include <stdnoreturn.h>

noreturn void panic(char const *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
