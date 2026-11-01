#ifndef ASSERT_H
#define ASSERT_H

void __assert_fail(char const *file, int line, char const *expr)
	__attribute__((noreturn));

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
