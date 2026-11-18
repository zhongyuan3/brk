#ifndef UAPI_TYPES_H
#define UAPI_TYPES_H

/*
 * Fixed-width types for the kernel/user ABI.
 *
 * This header is self-contained: it must compile with -nostdinc and must not
 * depend on <stdint.h> or on any kernel-internal header.  Only compiler
 * predefined type macros are used.
 *
 * UAPI type names are deliberately in the reserved __brk_* namespace so that
 * a user program can include this header together with its libc headers
 * without colliding with u32, size_t, pid_t, etc.
 *
 * Kernel-internal code should include <brk/types.h>, which maps these types
 * to the conventional kernel spellings (u32, pid_t, off_t, ...).
 *
 * The C-standard names (u32, size_t, ...) live in
 * <stdint.h> and are intentionally separate typedefs: both use
 * the same compiler predefined type macros, and src/kernel/abi_check.c
 * asserts that the two spellings remain type-compatible.  Do not include
 * <stdint.h> from this header: UAPI must stay self-contained.
 */

typedef __UINT8_TYPE__ __brk_u8;
typedef __UINT16_TYPE__ __brk_u16;
typedef __UINT32_TYPE__ __brk_u32;
typedef __UINT64_TYPE__ __brk_u64;

typedef __INT8_TYPE__ __brk_s8;
typedef __INT16_TYPE__ __brk_s16;
typedef __INT32_TYPE__ __brk_s32;
typedef __INT64_TYPE__ __brk_s64;

/*
 * long is 64-bit on RV64 and 32-bit on RV32.  Keep the ABI definitions
 * explicit instead of relying on the C implementation's "long".
 */
#if __SIZEOF_LONG__ == 8
typedef __brk_s64 __brk_long_t;
typedef __brk_u64 __brk_ulong_t;
#else
typedef __brk_s32 __brk_long_t;
typedef __brk_u32 __brk_ulong_t;
#endif

typedef __SIZE_TYPE__ __brk_size_t;
typedef __PTRDIFF_TYPE__ __brk_ssize_t;
typedef __PTRDIFF_TYPE__ __brk_ptrdiff_t;

typedef __brk_long_t __brk_time_t;
typedef __brk_long_t __brk_suseconds_t;
typedef __brk_long_t __brk_clock_t;

typedef __brk_s32 __brk_pid_t;
typedef __brk_u32 __brk_uid_t;
typedef __brk_u32 __brk_gid_t;

/* dev_t is 32-bit in the kernel even though struct stat stores it widened. */
typedef __brk_u32 __brk_dev_t;
typedef __brk_u32 __brk_mode_t;
typedef __brk_u32 __brk_nlink_t;
typedef __brk_u64 __brk_ino_t;

typedef __brk_long_t __brk_off_t;
typedef __brk_long_t __brk_loff_t;
typedef __brk_long_t __brk_blkcnt_t;
typedef __brk_long_t __brk_blksize_t;

typedef __brk_u16 __brk_sa_family_t;
typedef __brk_u32 __brk_socklen_t;

#endif
