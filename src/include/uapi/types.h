#ifndef UAPI_TYPES_H
#define UAPI_TYPES_H

/*
 * Fixed-width types for the kernel/user ABI.
 *
 * This header is self-contained: it must compile with -nostdinc and must not
 * depend on <stdint.h> or on any kernel-internal header.  Only compiler
 * predefined type macros are used.
 *
 * UAPI type names are deliberately in the reserved __u* / __kernel_*
 * namespace so that a user program can include this header together with its
 * libc headers without colliding with u32, size_t, pid_t, etc.
 *
 * Kernel-internal code should include <brk/types.h>, which maps these types
 * to the conventional kernel spellings (u32, pid_t, off_t, ...).
 *
 * The C-standard names (u32, size_t, ...) live in
 * <freestanding/stdint.h> and are intentionally separate typedefs: both use
 * the same compiler predefined type macros, and src/kernel/abi_check.c
 * asserts that the two spellings remain type-compatible.  Do not include
 * <stdint.h> from this header: UAPI must stay self-contained.
 */

typedef __UINT8_TYPE__ __u8;
typedef __UINT16_TYPE__ __u16;
typedef __UINT32_TYPE__ __u32;
typedef __UINT64_TYPE__ __u64;

typedef __INT8_TYPE__ __s8;
typedef __INT16_TYPE__ __s16;
typedef __INT32_TYPE__ __s32;
typedef __INT64_TYPE__ __s64;

/*
 * long is 64-bit on RV64 and 32-bit on RV32.  Keep the ABI definitions
 * explicit instead of relying on the C implementation's "long".
 */
#if __SIZEOF_LONG__ == 8
typedef __s64 __kernel_long_t;
typedef __u64 __kernel_ulong_t;
#else
typedef __s32 __kernel_long_t;
typedef __u32 __kernel_ulong_t;
#endif

typedef __SIZE_TYPE__ __kernel_size_t;
typedef __PTRDIFF_TYPE__ __kernel_ssize_t;
typedef __PTRDIFF_TYPE__ __kernel_ptrdiff_t;

typedef __kernel_long_t __kernel_time_t;
typedef __kernel_long_t __kernel_suseconds_t;
typedef __kernel_long_t __kernel_clock_t;

typedef __s32 __kernel_pid_t;
typedef __u32 __kernel_uid_t;
typedef __u32 __kernel_gid_t;

/* dev_t is 32-bit in the kernel even though struct stat stores it widened. */
typedef __u32 __kernel_dev_t;
typedef __u32 __kernel_mode_t;
typedef __u32 __kernel_nlink_t;
typedef __u64 __kernel_ino_t;

typedef __kernel_long_t __kernel_off_t;
typedef __kernel_long_t __kernel_loff_t;
typedef __kernel_long_t __kernel_blkcnt_t;
typedef __kernel_long_t __kernel_blksize_t;

typedef __u16 __kernel_sa_family_t;
typedef __u32 __kernel_socklen_t;

#endif
