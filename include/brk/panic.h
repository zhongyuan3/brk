#ifndef _BRK_PANIC_H
#define _BRK_PANIC_H

#include <brk/compiler_attr.h>
#include <stdnoreturn.h>

noreturn void panic(char const *fmt, ...) __printf(1, 2);

#endif
