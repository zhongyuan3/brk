#ifndef BRK_ASSERT_H
#define BRK_ASSERT_H

void assert_fail(char const *file, int line, char const *expr)
	__attribute__((noreturn));

#ifndef NDEBUG
#define assert(expr)                                            \
	do {                                                    \
		if (!(expr))                                    \
			assert_fail(__FILE__, __LINE__, #expr); \
	} while (0)
#else
#define assert(expr) ((void)0)
#endif

#endif
