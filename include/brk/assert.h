#ifndef BRK_ASSERT_H
#define BRK_ASSERT_H

#include <stdnoreturn.h>

noreturn void __assert_fail(char const *file, int line, char const *expr);

#ifndef NDEBUG
#define assert(expr)                                              \
	do {                                                      \
		if (!(expr))                                      \
			__assert_fail(__FILE__, __LINE__, #expr); \
	} while (0)
#else
#define assert(expr) ((void)0)
#endif

#define static_assert _Static_assert

#endif
