#ifndef BRK_TYPES_H
#define BRK_TYPES_H

#include <uapi/types.h>

/*
 * Kernel-visible spellings of the UAPI ABI types.  Userland owns its own
 * typedefs; the kernel maps the __brk_* forms here so that internal code
 * does not need to include libc headers.
 */
typedef __brk_dev_t dev_t;
typedef __brk_mode_t mode_t;
typedef __brk_nlink_t nlink_t;
typedef __brk_ino_t ino_t;
typedef __brk_off_t off_t;
typedef __brk_loff_t loff_t;
typedef __brk_pid_t pid_t;
typedef __brk_clock_t clock_t;
typedef __brk_suseconds_t suseconds_t;
typedef __brk_time_t time_t;
typedef __brk_uid_t uid_t;
typedef __brk_gid_t gid_t;
typedef __brk_ssize_t ssize_t;

/* Fixed-width aliases used by kernel-internal code. */
typedef __brk_u8 u8;
typedef __brk_u16 u16;
typedef __brk_u32 u32;
typedef __brk_u64 u64;
typedef __brk_s8 s8;
typedef __brk_s16 s16;
typedef __brk_s32 s32;
typedef __brk_s64 s64;

/* Kernel-internal time, always nanoseconds. */
typedef __brk_s64 ktime_t;

typedef unsigned long cpuid_t;
typedef unsigned int umode_t;
typedef unsigned int kuid_t;
typedef unsigned int kgid_t;

/* Signal mask bit set; one bit per signal number. */
typedef __brk_ulong_t sigset_t;

/*
 * Address spaces are deliberately separated so that address arithmetic
 * cannot be mixed up accidentally.  Architecture code may override the
 * width of the physical and DMA addresses; virtual and user addresses
 * always follow the pointer width.
 */

/** @brief Physical address. */
typedef __UINT64_TYPE__ phys_addr_t;

/** @brief Size in the physical address space. */
typedef __UINT64_TYPE__ phys_size_t;

/** @brief Size stored in a struct resource. */
typedef __UINT64_TYPE__ resource_size_t;

/** @brief DMA/device address. */
typedef __UINT64_TYPE__ dma_addr_t;

/** @brief Physical page frame number. */
typedef __UINT64_TYPE__ pfn_t;

/** @brief Integer kernel virtual address, used for address arithmetic. */
typedef __UINTPTR_TYPE__ virt_addr_t;

/** @brief Integer user virtual address. */
typedef __UINTPTR_TYPE__ user_addr_t;

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
