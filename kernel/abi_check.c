#include <stddef.h>

#include <brk/types.h>
#include <stdint.h>
#include <uapi/capability.h>
#include <uapi/dirent.h>
#include <uapi/elf.h>
#include <uapi/ioctl.h>
#include <uapi/poll.h>
#include <uapi/proc.h>
#include <uapi/resource.h>
#include <uapi/signal.h>
#include <uapi/socket.h>
#include <uapi/stat.h>
#include <uapi/statvfs.h>
#include <uapi/sysinfo.h>
#include <uapi/termios.h>
#include <uapi/time.h>
#include <uapi/times.h>
#include <uapi/utsname.h>

_Static_assert(sizeof(ktime_t) == 8, "ktime_t is not 64-bit");

_Static_assert(sizeof(phys_addr_t) == 8, "phys_addr_t is not 64-bit");
_Static_assert(sizeof(phys_size_t) == 8, "phys_size_t is not 64-bit");
_Static_assert(sizeof(resource_size_t) == 8, "resource_size_t is not 64-bit");
_Static_assert(sizeof(dma_addr_t) == 8, "dma_addr_t is not 64-bit");
_Static_assert(sizeof(pfn_t) == 8, "pfn_t is not 64-bit");
_Static_assert(sizeof(virt_addr_t) == sizeof(void *),
	       "virt_addr_t is not pointer-sized");
_Static_assert(sizeof(user_addr_t) == sizeof(void *),
	       "user_addr_t is not pointer-sized");

_Static_assert(__builtin_types_compatible_p(sigset_t, __brk_sigset_t),
	       "sigset_t and __brk_sigset_t differ");
_Static_assert(sizeof(__brk_sigset_t) == 8, "__brk_sigset_t is not 64-bit");
_Static_assert(sizeof(__brk_fd_mask_t) == 8, "__brk_fd_mask_t is not 64-bit");
_Static_assert(sizeof(__brk_nfds_t) == 8, "__brk_nfds_t is not 64-bit");
_Static_assert(sizeof(__brk_cc_t) == 1, "__brk_cc_t is not 8-bit");
_Static_assert(sizeof(__brk_speed_t) == 4, "__brk_speed_t is not 32-bit");
_Static_assert(sizeof(__brk_tcflag_t) == 4, "__brk_tcflag_t is not 32-bit");

_Static_assert(__builtin_types_compatible_p(s8, __brk_s8),
	       "s8 and __brk_s8 differ");
_Static_assert(__builtin_types_compatible_p(s16, __brk_s16),
	       "s16 and __brk_s16 differ");
_Static_assert(__builtin_types_compatible_p(s32, __brk_s32),
	       "s32 and __brk_s32 differ");
_Static_assert(__builtin_types_compatible_p(s64, __brk_s64),
	       "s64 and __brk_s64 differ");
_Static_assert(__builtin_types_compatible_p(u8, __brk_u8),
	       "u8 and __brk_u8 differ");
_Static_assert(__builtin_types_compatible_p(u16, __brk_u16),
	       "u16 and __brk_u16 differ");
_Static_assert(__builtin_types_compatible_p(u32, __brk_u32),
	       "u32 and __brk_u32 differ");
_Static_assert(__builtin_types_compatible_p(u64, __brk_u64),
	       "u64 and __brk_u64 differ");
_Static_assert(__builtin_types_compatible_p(size_t, __brk_size_t),
	       "size_t and __brk_size_t differ");
_Static_assert(__builtin_types_compatible_p(ptrdiff_t, __brk_ssize_t),
	       "ptrdiff_t and __brk_ssize_t differ");
_Static_assert(__builtin_types_compatible_p(uintptr_t, __brk_ulong_t),
	       "uintptr_t and __brk_ulong_t differ");

_Static_assert(sizeof(struct stat) == 128, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_ino) == 8, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_mode) == 16, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_size) == 48, "struct stat ABI changed");
_Static_assert(offsetof(struct stat, st_atime) == 72,
	       "struct stat ABI changed");

_Static_assert(sizeof(struct statvfs) == 88, "struct statvfs ABI changed");
_Static_assert(sizeof(struct timespec) == 16, "struct timespec ABI changed");
_Static_assert(sizeof(struct timeval) == 16, "struct timeval ABI changed");
_Static_assert(sizeof(struct rusage) == 144, "struct rusage ABI changed");
_Static_assert(sizeof(struct sigaction) == 24, "struct sigaction ABI changed");
_Static_assert(sizeof(struct tms) == 32, "struct tms ABI changed");

_Static_assert(sizeof(struct termios) == 60, "struct termios ABI changed");
_Static_assert(offsetof(struct termios, c_cc) == 17,
	       "struct termios ABI changed");
_Static_assert(offsetof(struct termios, c_ispeed) == 52,
	       "struct termios ABI changed");
_Static_assert(offsetof(struct termios, c_ospeed) == 56,
	       "struct termios ABI changed");

_Static_assert(sizeof(ino_t) == 8, "ino_t is not 64-bit");
_Static_assert(sizeof(struct brk_dirent) == 16,
	       "struct brk_dirent ABI changed");
_Static_assert(offsetof(struct brk_dirent, d_name) == 13,
	       "struct brk_dirent ABI changed");

_Static_assert(sizeof(struct brk_proc) == 176,
	       "struct brk_proc ABI changed");
_Static_assert(offsetof(struct brk_proc, tid) == 16,
	       "struct brk_proc ABI changed");
_Static_assert(offsetof(struct brk_proc, suid) == 28,
	       "struct brk_proc ABI changed");
_Static_assert(offsetof(struct brk_proc, sgid) == 40,
	       "struct brk_proc ABI changed");
_Static_assert(offsetof(struct brk_proc, ngroups) == 44,
	       "struct brk_proc ABI changed");
_Static_assert(offsetof(struct brk_proc, groups) == 48,
	       "struct brk_proc ABI changed");

_Static_assert(sizeof(struct brk_sysinfo) == 224,
	       "struct brk_sysinfo ABI changed");
_Static_assert(offsetof(struct brk_sysinfo, page_size) == 196,
	       "struct brk_sysinfo ABI changed");
_Static_assert(offsetof(struct brk_sysinfo, mono_resolution_ns) == 208,
	       "struct brk_sysinfo ABI changed");

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
