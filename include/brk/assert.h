#ifndef _BRK_ASSERT_H
#define _BRK_ASSERT_H

#include <assert.h>
#include <stdnoreturn.h>

noreturn void __kassert_fail(char const *file, int line, char const *expr);

#ifndef NDEBUG
#define kassert(expr)                                              \
	do {                                                       \
		if (!(expr))                                       \
			__kassert_fail(__FILE__, __LINE__, #expr); \
	} while (0)
#else
#define kassert(expr) ((void)0)
#endif

#endif
