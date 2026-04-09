#ifndef BRK_ALIGN_H
#define BRK_ALIGN_H

#include <stdalign.h>

#define align_up(x, align)                            \
	({                                            \
		__auto_type __x = (x);                \
		__auto_type __align = (align);        \
		(__x + __align - 1) & ~(__align - 1); \
	})

#define align_down(x, align)                   \
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

#define align_up_to_pow2(x)                          \
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

#endif