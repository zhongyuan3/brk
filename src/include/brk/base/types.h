#ifndef BRK_TYPES_H
#define BRK_TYPES_H

#include <uapi/brk/types.h>

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
