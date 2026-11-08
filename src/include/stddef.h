#ifndef STDDEF_H
#define STDDEF_H

#include <stdalign.h>

typedef __SIZE_TYPE__ size_t;

#define NULL ((void *)0)

#define offsetof(type, member) __builtin_offsetof(type, member)

typedef __PTRDIFF_TYPE__ ptrdiff_t;

/* Type with the strictest alignment requirement on the target. */
typedef struct {
	alignas(alignof(long long)) long long __max_align_ll;
	alignas(alignof(long double)) long double __max_align_ld;
} max_align_t;

#endif
