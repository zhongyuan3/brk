#ifndef BRK_STDARGS_H
#define BRK_STDARGS_H

/*
 * Freestanding replacement for <stdarg.h>.  GCC exposes the va_list type
 * and the va_* operations as builtins, so no compiler-provided header is
 * needed.
 */

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst, src) __builtin_va_copy(dst, src)

#endif
