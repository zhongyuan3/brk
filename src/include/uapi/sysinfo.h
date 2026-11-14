#ifndef UAPI_SYSINFO_H
#define UAPI_SYSINFO_H

#include <uapi/types.h>

/*
 * Stable kernel information snapshot.  libc derives uname() and
 * clock_getres() from this structure.  Padding is explicit so the layout does
 * not depend on compiler-specific tail padding.
 */
struct brk_sysinfo {
	char sysname[65];
	char release[65];
	char machine[65];
	__u8 __pad0; /* pad to 4-byte alignment */
	__u32 page_size;
	__u32 hz;
	__u32 __pad1; /* pad to 8-byte alignment */
	__u64 mono_resolution_ns;
	__u64 cpu_resolution_ns;
};

#endif
