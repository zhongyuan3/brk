#ifndef BRK_KERNEL_H
#define BRK_KERNEL_H

#include <stdalign.h>
#include <stdint.h>

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

#define is_power_of_two(x)                          \
	({                                          \
		__auto_type __x = (x);              \
		__x != 0 && (__x & (__x - 1)) == 0; \
	})

#define round_up(x, align)                            \
	({                                            \
		__auto_type __x = (x);                \
		__auto_type __align = (align);        \
		(__x + __align - 1) & ~(__align - 1); \
	})

#define round_down(x, align)                   \
	({                                     \
		__auto_type __x = (x);         \
		__auto_type __align = (align); \
		(__x & ~(__align - 1));        \
	})

#define is_aligned(x, align)                   \
	({                                     \
		__auto_type __x = (x);         \
		__auto_type __align = (align); \
		(__x & (__align - 1)) == 0;    \
	})

#define round_up_to_pow_of_two(x)                    \
	({                                           \
		__auto_type __x = (x);               \
		if (__x == 0) {                      \
			__x = 1;                     \
		} else if ((__x & (__x - 1)) == 0) { \
		} else {                             \
			--__x;                       \
			__x |= __x >> 1;             \
			__x |= __x >> 2;             \
			__x |= __x >> 4;             \
			__x |= __x >> 8;             \
			__x |= __x >> 16;            \
			if (sizeof(__x) == 8)        \
				__x |= __x >> 32;    \
			++__x;                       \
		}                                    \
		__x;                                 \
	})

#define div_ceil(num, div)                   \
	({                                   \
		__auto_type __num = (num);   \
		__auto_type __div = (div);   \
		(__num + __div - 1) / __div; \
	})

#endif
