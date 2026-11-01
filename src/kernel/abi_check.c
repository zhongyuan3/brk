#include <stddef.h>

#include <brk/types.h>
#include <uapi/capability.h>
#include <uapi/dirent.h>
#include <uapi/elf.h>
#include <uapi/ioctl.h>
#include <uapi/poll.h>
#include <uapi/resource.h>
#include <uapi/signal.h>
#include <uapi/socket.h>
#include <uapi/stat.h>
#include <uapi/statfs.h>
#include <uapi/termios.h>
#include <uapi/time.h>
#include <uapi/times.h>
#include <uapi/utsname.h>

/*
 * These assertions are part of the kernel build.  Any change to a UAPI
 * structure that is visible to userland must be intentional, so it must also
 * update (or explicitly invalidate) the ABI contract here.
 *
 * Layouts below are for RV64 (LP64).  Add per-architecture values here when
 * another ABI is supported.
 */

_Static_assert(sizeof(ktime_t) == 8, "ktime_t is not 64-bit");

/*
 * freestanding/stdint.h and uapi/types.h intentionally define separate
 * namespaces (u64 vs __u64), but they must name the same underlying
 * compiler types.  If one side changes its base type, this fails the build
 * instead of silently producing incompatible pointer types.
 */
_Static_assert(__builtin_types_compatible_p(s8, __s8), "s8 and __s8 differ");
_Static_assert(__builtin_types_compatible_p(s16, __s16),
	       "s16 and __s16 differ");
_Static_assert(__builtin_types_compatible_p(s32, __s32),
	       "s32 and __s32 differ");
_Static_assert(__builtin_types_compatible_p(s64, __s64),
	       "s64 and __s64 differ");
_Static_assert(__builtin_types_compatible_p(u8, __u8), "u8 and __u8 differ");
_Static_assert(__builtin_types_compatible_p(u16, __u16),
	       "u16 and __u16 differ");
_Static_assert(__builtin_types_compatible_p(u32, __u32),
	       "u32 and __u32 differ");
_Static_assert(__builtin_types_compatible_p(u64, __u64),
	       "u64 and __u64 differ");
_Static_assert(__builtin_types_compatible_p(size_t, __kernel_size_t),
	       "size_t and __kernel_size_t differ");
_Static_assert(__builtin_types_compatible_p(ptrdiff_t, __kernel_ssize_t),
	       "ptrdiff_t and __kernel_ssize_t differ");
_Static_assert(__builtin_types_compatible_p(uintptr_t, __kernel_ulong_t),
	       "uintptr_t and __kernel_ulong_t differ");

_Static_assert(sizeof(struct stat) == 128, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_ino) == 8, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_mode) == 16, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_size) == 48, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_atime) == 72,
	       "struct stat ABI changed");

_Static_assert(sizeof(struct statfs) == 120, "struct statfs ABI changed");
_Static_assert(sizeof(struct timespec) == 16, "struct timespec ABI changed");
_Static_assert(sizeof(struct timeval) == 16, "struct timeval ABI changed");
_Static_assert(sizeof(struct rusage) == 144, "struct rusage ABI changed");
_Static_assert(sizeof(struct sigaction) == 24, "struct sigaction ABI changed");
_Static_assert(sizeof(struct tms) == 32, "struct tms ABI changed");

_Static_assert(sizeof(struct dirent) == 268, "struct dirent ABI changed");
_Static_assert(sizeof(struct dirent64) == 280, "struct dirent64 ABI changed");

_Static_assert(sizeof(struct elf_hdr) == 64, "struct elf_hdr ABI changed");
_Static_assert(sizeof(struct elf_phdr) == 56, "struct elf_phdr ABI changed");

_Static_assert(sizeof(struct utsname) == 390, "struct utsname ABI changed");
_Static_assert(sizeof(struct cap_header) == 8, "struct cap_header ABI changed");
_Static_assert(sizeof(struct cap_data) == 12, "struct cap_data ABI changed");
_Static_assert(sizeof(struct winsize) == 8, "struct winsize ABI changed");
_Static_assert(sizeof(struct sockaddr_in) == 16,
	       "struct sockaddr_in ABI changed");
_Static_assert(sizeof(struct pollfd) == 8, "struct pollfd ABI changed");
_Static_assert(sizeof(struct termios) == 60, "struct termios ABI changed");
