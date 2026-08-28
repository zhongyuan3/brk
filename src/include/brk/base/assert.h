#ifndef BRK_ASSERT_H
#define BRK_ASSERT_H

void __assert_fail(char const *file, int line, char const *expr)
	__attribute__((noreturn));

#ifndef NDEBUG
#define ASSERT(expr)                                              \
	do {                                                      \
		if (!(expr))                                      \
			__assert_fail(__FILE__, __LINE__, #expr); \
	} while (0)
#else
#define ASSERT(expr) ((void)0)
#endif

#endif
