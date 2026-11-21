#ifndef UAPI_STATVFS_H
#define UAPI_STATVFS_H

#include <uapi/types.h>

/* f_flag bits */
#define ST_RDONLY 0x0001
#define ST_NOSUID 0x0002

/*
 * Kernel-side filesystem statistics primitive.  libc converts it into the
 * POSIX struct statvfs layout.
 */
struct __brk_statfs {
	__brk_s64 type;
	__brk_s64 bsize;
	__brk_s64 frsize;
	__brk_s64 blocks;
	__brk_s64 bfree;
	__brk_s64 bavail;
	__brk_s64 files;
	__brk_s64 ffree;
	__brk_s64 fsid;
	__brk_s64 namelen;
	__brk_s64 flags;
};


#endif
