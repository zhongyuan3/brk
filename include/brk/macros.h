#ifndef BRK_MACROS_H
#define BRK_MACROS_H

#define is_pow2(x)                                  \
	({                                          \
		__auto_type __x = (x);              \
		__x != 0 && (__x & (__x - 1)) == 0; \
	})

#define max(x, y)                      \
	({                             \
		__auto_type __x = (x); \
		__auto_type __y = (y); \
		__x > __y ? __x : __y; \
	})

#define min(x, y)                      \
	({                             \
		__auto_type __x = (x); \
		__auto_type __y = (y); \
		__x < __y ? __x : __y; \
	})

#ifndef countof
#define countof(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define container_of(ptr, type, member)                              \
	({                                                           \
		typeof((((type *)0)->member)) const *__mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member));   \
	})

#endif
