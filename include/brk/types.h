#ifndef BRK_TYPES_H
#define BRK_TYPES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct list_head {
	struct list_head *prev, *next;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

typedef unsigned int dev_t;
typedef unsigned int mode_t;
typedef unsigned int fmode_t;
typedef long off_t;
typedef long pid_t;
typedef int cpuid_t;
typedef long clock_t;
typedef long suseconds_t;

typedef unsigned int uid_t;
typedef unsigned int gid_t;

typedef unsigned int umode_t;
typedef long loff_t;
typedef long ssize_t;

#endif
