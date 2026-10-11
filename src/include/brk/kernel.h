#ifndef BRK_KERNEL_H
#define BRK_KERNEL_H

#include <stdalign.h>
#include <stdint.h>

#define BRK_VERSION "0.0.1"

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
		unsigned long long __v = __x;        \
		if (__v == 0) {                      \
			__v = 1;                     \
		} else if ((__v & (__v - 1)) == 0) { \
		} else {                             \
			--__v;                       \
			__v |= __v >> 1;             \
			__v |= __v >> 2;             \
			__v |= __v >> 4;             \
			__v |= __v >> 8;             \
			__v |= __v >> 16;            \
			__v |= __v >> 32;            \
			++__v;                       \
		}                                    \
		__x = (__typeof__(__x))__v;          \
		__x;                                 \
	})

#define div_ceil(num, div)                   \
	({                                   \
		__auto_type __num = (num);   \
		__auto_type __div = (div);   \
		(__num + __div - 1) / __div; \
	})

#define round_up_pow2_const(x, pow2) (((x) + (pow2) - 1) & ~((pow2) - 1))

#define round_down_pow2_const(x, pow2) ((x) & ~((pow2) - 1))

#endif
