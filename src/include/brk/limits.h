#ifndef BRK_LIMITS_H
#define BRK_LIMITS_H

/*
 * Freestanding replacement for <limits.h> and the fixed-width limits that
 * <stdint.h> normally provides.  Everything is derived from the compiler's
 * predefined limit macros, so it tracks the target ABI automatically.
 */

#define CHAR_BIT 8

#define SCHAR_MIN (-__SCHAR_MAX__ - 1)
#define SCHAR_MAX __SCHAR_MAX__
#define UCHAR_MAX (__SCHAR_MAX__ * 2 + 1)

#define SHRT_MIN (-__SHRT_MAX__ - 1)
#define SHRT_MAX __SHRT_MAX__
#define USHRT_MAX (__SHRT_MAX__ * 2 + 1)

#define INT_MIN (-__INT_MAX__ - 1)
#define INT_MAX __INT_MAX__
#define UINT_MAX (__INT_MAX__ * 2U + 1U)

#define LONG_MIN (-__LONG_MAX__ - 1L)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX (__LONG_MAX__ * 2UL + 1UL)

#define LLONG_MIN (-__LONG_LONG_MAX__ - 1LL)
#define LLONG_MAX __LONG_LONG_MAX__
#define ULLONG_MAX (__LONG_LONG_MAX__ * 2ULL + 1ULL)

#define PTRDIFF_MIN (-__PTRDIFF_MAX__ - 1)
#define PTRDIFF_MAX __PTRDIFF_MAX__
#define SIZE_MAX __SIZE_MAX__

#define INT8_MIN (-__INT8_MAX__ - 1)
#define INT8_MAX __INT8_MAX__
#define UINT8_MAX __UINT8_MAX__

#define INT16_MIN (-__INT16_MAX__ - 1)
#define INT16_MAX __INT16_MAX__
#define UINT16_MAX __UINT16_MAX__

#define INT32_MIN (-__INT32_MAX__ - 1)
#define INT32_MAX __INT32_MAX__
#define UINT32_MAX __UINT32_MAX__

#define INT64_MIN (-__INT64_MAX__ - 1)
#define INT64_MAX __INT64_MAX__
#define UINT64_MAX __UINT64_MAX__

#define INTPTR_MIN (-__INTPTR_MAX__ - 1)
#define INTPTR_MAX __INTPTR_MAX__
#define UINTPTR_MAX __UINTPTR_MAX__

#define INTMAX_MIN (-__INTMAX_MAX__ - 1)
#define INTMAX_MAX __INTMAX_MAX__
#define UINTMAX_MAX __UINTMAX_MAX__

#endif
