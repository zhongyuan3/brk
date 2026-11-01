#ifndef BRK_TYPES_H
#define BRK_TYPES_H

#include <brk/assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/types.h>

/*
 * Kernel-visible spellings of the UAPI ABI types.  Userland owns its own
 * typedefs; the kernel maps the __kernel_* forms here so that internal code
 * does not need to include libc headers.
 */
typedef __kernel_dev_t dev_t;
typedef __kernel_mode_t mode_t;
typedef __kernel_nlink_t nlink_t;
typedef __kernel_ino_t ino_t;
typedef __kernel_off_t off_t;
typedef __kernel_loff_t loff_t;
typedef __kernel_pid_t pid_t;
typedef __kernel_clock_t clock_t;
typedef __kernel_suseconds_t suseconds_t;
typedef __kernel_time_t time_t;
typedef __kernel_uid_t uid_t;
typedef __kernel_gid_t gid_t;
typedef __kernel_ssize_t ssize_t;

/* Fixed-width aliases used by kernel-internal code. */
typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s8 s8;
typedef __s16 s16;
typedef __s32 s32;
typedef __s64 s64;

/* Kernel-internal time, always nanoseconds. */
typedef __s64 ktime_t;

typedef unsigned int fmode_t;
typedef unsigned long cpuid_t;
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
