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

struct statvfs {
	__brk_ulong_t f_bsize; /* Filesystem block size */
	__brk_ulong_t f_frsize; /* Fragment size */
	__brk_ulong_t f_blocks; /* Number of blocks */
	__brk_ulong_t f_bfree; /* Free blocks */
	__brk_ulong_t f_bavail; /* Free blocks for unprivileged users */
	__brk_ulong_t f_files; /* Number of inodes */
	__brk_ulong_t f_ffree; /* Free inodes */
	__brk_ulong_t f_favail; /* Free inodes for unprivileged users */
	__brk_ulong_t f_fsid; /* Filesystem ID */
	__brk_ulong_t f_flag; /* Mount flags */
	__brk_ulong_t f_namemax; /* Maximum filename length */
};

#endif
