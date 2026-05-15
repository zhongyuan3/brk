#ifndef BRK_TYPES_H
#define BRK_TYPES_H

#include <uapi/types.h>

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef size_t usize_t;

typedef unsigned int fmode_t;
typedef int cpuid_t;
typedef unsigned int umode_t;
typedef unsigned int kuid_t;
typedef unsigned int kgid_t;

struct list_head {
	struct list_head *prev, *next;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

#endif
