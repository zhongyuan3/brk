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
struct brk_statfs {
	__s64 type;
	__s64 bsize;
	__s64 frsize;
	__s64 blocks;
	__s64 bfree;
	__s64 bavail;
	__s64 files;
	__s64 ffree;
	__s64 fsid;
	__s64 namelen;
	__s64 flags;
};

struct statvfs {
	__kernel_ulong_t f_bsize; /* Filesystem block size */
	__kernel_ulong_t f_frsize; /* Fragment size */
	__kernel_ulong_t f_blocks; /* Number of blocks */
	__kernel_ulong_t f_bfree; /* Free blocks */
	__kernel_ulong_t f_bavail; /* Free blocks for unprivileged users */
	__kernel_ulong_t f_files; /* Number of inodes */
	__kernel_ulong_t f_ffree; /* Free inodes */
	__kernel_ulong_t f_favail; /* Free inodes for unprivileged users */
	__kernel_ulong_t f_fsid; /* Filesystem ID */
	__kernel_ulong_t f_flag; /* Mount flags */
	__kernel_ulong_t f_namemax; /* Maximum filename length */
};

#endif
