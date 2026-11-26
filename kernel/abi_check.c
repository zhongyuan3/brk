#include <assert.h>
#include <brk/types.h>
#include <stddef.h>
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

static_assert(sizeof(ktime_t) == 8, "ktime_t is not 64-bit");

static_assert(sizeof(phys_addr_t) == 8, "phys_addr_t is not 64-bit");
static_assert(sizeof(phys_size_t) == 8, "phys_size_t is not 64-bit");
static_assert(sizeof(resource_size_t) == 8, "resource_size_t is not 64-bit");
static_assert(sizeof(dma_addr_t) == 8, "dma_addr_t is not 64-bit");
static_assert(sizeof(pfn_t) == 8, "pfn_t is not 64-bit");
static_assert(sizeof(virt_addr_t) == sizeof(void *),
	      "virt_addr_t is not pointer-sized");
static_assert(sizeof(user_addr_t) == sizeof(void *),
	      "user_addr_t is not pointer-sized");

static_assert(__builtin_types_compatible_p(sigset_t, __brk_sigset_t),
	      "sigset_t and __brk_sigset_t differ");
static_assert(sizeof(__brk_sigset_t) == 8, "__brk_sigset_t is not 64-bit");
static_assert(sizeof(__brk_fd_mask_t) == 8, "__brk_fd_mask_t is not 64-bit");
static_assert(sizeof(__brk_nfds_t) == 8, "__brk_nfds_t is not 64-bit");
static_assert(sizeof(__brk_cc_t) == 1, "__brk_cc_t is not 8-bit");
static_assert(sizeof(__brk_speed_t) == 4, "__brk_speed_t is not 32-bit");
static_assert(sizeof(__brk_tcflag_t) == 4, "__brk_tcflag_t is not 32-bit");

static_assert(__builtin_types_compatible_p(s8, __brk_s8),
	      "s8 and __brk_s8 differ");
static_assert(__builtin_types_compatible_p(s16, __brk_s16),
	      "s16 and __brk_s16 differ");
static_assert(__builtin_types_compatible_p(s32, __brk_s32),
	      "s32 and __brk_s32 differ");
static_assert(__builtin_types_compatible_p(s64, __brk_s64),
	      "s64 and __brk_s64 differ");
static_assert(__builtin_types_compatible_p(u8, __brk_u8),
	      "u8 and __brk_u8 differ");
static_assert(__builtin_types_compatible_p(u16, __brk_u16),
	      "u16 and __brk_u16 differ");
static_assert(__builtin_types_compatible_p(u32, __brk_u32),
	      "u32 and __brk_u32 differ");
static_assert(__builtin_types_compatible_p(u64, __brk_u64),
	      "u64 and __brk_u64 differ");
static_assert(__builtin_types_compatible_p(size_t, __brk_size_t),
	      "size_t and __brk_size_t differ");
static_assert(__builtin_types_compatible_p(ptrdiff_t, __brk_ssize_t),
	      "ptrdiff_t and __brk_ssize_t differ");
static_assert(__builtin_types_compatible_p(uintptr_t, __brk_ulong_t),
	      "uintptr_t and __brk_ulong_t differ");

static_assert(sizeof(struct __brk_timespec) == 16,
	      "struct __brk_timespec ABI changed");
static_assert(sizeof(struct __brk_timeval) == 16,
	      "struct __brk_timeval ABI changed");
static_assert(sizeof(struct __brk_rusage) == 144,
	      "struct __brk_rusage ABI changed");
static_assert(sizeof(struct __brk_sigaction) == 24,
	      "struct __brk_sigaction ABI changed");

static_assert(sizeof(struct __brk_termios) == 60,
	      "struct __brk_termios ABI changed");
static_assert(offsetof(struct __brk_termios, c_cc) == 17,
	      "struct __brk_termios ABI changed");
static_assert(offsetof(struct __brk_termios, c_ispeed) == 52,
	      "struct __brk_termios ABI changed");
static_assert(offsetof(struct __brk_termios, c_ospeed) == 56,
	      "struct __brk_termios ABI changed");

static_assert(sizeof(ino_t) == 8, "ino_t is not 64-bit");
static_assert(sizeof(struct __brk_dirent) == 16,
	      "struct __brk_dirent ABI changed");
static_assert(offsetof(struct __brk_dirent, d_name) == 13,
	      "struct __brk_dirent ABI changed");

static_assert(sizeof(struct __brk_proc) == 176,
	      "struct __brk_proc ABI changed");
static_assert(offsetof(struct __brk_proc, tid) == 16,
	      "struct __brk_proc ABI changed");
static_assert(offsetof(struct __brk_proc, suid) == 28,
	      "struct __brk_proc ABI changed");
static_assert(offsetof(struct __brk_proc, sgid) == 40,
	      "struct __brk_proc ABI changed");
static_assert(offsetof(struct __brk_proc, ngroups) == 44,
	      "struct __brk_proc ABI changed");
static_assert(offsetof(struct __brk_proc, groups) == 48,
	      "struct __brk_proc ABI changed");

static_assert(sizeof(struct __brk_sysinfo) == 224,
	      "struct __brk_sysinfo ABI changed");
static_assert(offsetof(struct __brk_sysinfo, page_size) == 196,
	      "struct __brk_sysinfo ABI changed");
static_assert(offsetof(struct __brk_sysinfo, mono_resolution_ns) == 208,
	      "struct __brk_sysinfo ABI changed");

static_assert(sizeof(struct __brk_elf_hdr) == 64,
	      "struct __brk_elf_hdr ABI changed");
static_assert(sizeof(struct __brk_elf_phdr) == 56,
	      "struct __brk_elf_phdr ABI changed");
static_assert(sizeof(struct __brk_elf_dyn) == 16,
	      "struct __brk_elf_dyn ABI changed");
static_assert(sizeof(struct __brk_elf_rela) == 24,
	      "struct __brk_elf_rela ABI changed");

static_assert(sizeof(struct __brk_cap_header) == 8,
	      "struct __brk_cap_header ABI changed");
static_assert(sizeof(struct __brk_cap_data) == 12,
	      "struct __brk_cap_data ABI changed");
static_assert(sizeof(struct __brk_winsize) == 8,
	      "struct __brk_winsize ABI changed");
static_assert(sizeof(struct __brk_sockaddr_in) == 16,
	      "struct __brk_sockaddr_in ABI changed");
static_assert(sizeof(struct __brk_pollfd) == 8,
	      "struct __brk_pollfd ABI changed");
static_assert(sizeof(struct __brk_termios) == 60,
	      "struct __brk_termios ABI changed");
