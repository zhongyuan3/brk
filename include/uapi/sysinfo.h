#ifndef _UAPI_SYSINFO_H
#define _UAPI_SYSINFO_H

#include <uapi/types.h>

/*
 * Stable kernel information snapshot.  libc derives uname() and
 * clock_getres() from this structure.  Padding is explicit so the layout does
 * not depend on compiler-specific tail padding.
 */
struct __brk_sysinfo {
	char sysname[65];
	char release[65];
	char machine[65];
	__brk_u8 __pad0; /* pad to 4-byte alignment */
	__brk_u32 page_size;
	__brk_u32 hz;
	__brk_u32 __pad1; /* pad to 8-byte alignment */
	__brk_u64 mono_resolution_ns;
	__brk_u64 cpu_resolution_ns;
};

#endif
