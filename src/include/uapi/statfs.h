#ifndef UAPI_STATFS_H
#define UAPI_STATFS_H

#include <uapi/types.h>

struct statfs {
	__kernel_long_t f_type;
	__kernel_long_t f_bsize;
	__kernel_long_t f_blocks;
	__kernel_long_t f_bfree;
	__kernel_long_t f_bavail;
	__kernel_long_t f_files;
	__kernel_long_t f_ffree;
	__kernel_long_t f_fsid;
	__kernel_long_t f_namelen;
	__kernel_long_t f_frsize;
	__kernel_long_t f_flags;
	__kernel_long_t f_spare[4];
};

#endif
