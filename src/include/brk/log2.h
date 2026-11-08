#ifndef BRK_LOG2_H
#define BRK_LOG2_H

#define is_power_of_2(x)                            \
	({                                          \
		__auto_type __x = (x);              \
		__x != 0 && (__x & (__x - 1)) == 0; \
	})

#define round_up_pow_of_2(x)                         \
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

#define round_up_pow_of_2_const(x, pow2) (((x) + (pow2) - 1) & ~((pow2) - 1))

#define round_down_pow_of_2_const(x, pow2) ((x) & ~((pow2) - 1))

#endif
