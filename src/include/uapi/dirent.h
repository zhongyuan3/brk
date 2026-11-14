#ifndef UAPI_DIRENT_H
#define UAPI_DIRENT_H

#include <uapi/types.h>

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

/*
 * Kernel-side directory entry primitive.  libc converts this compact layout
 * into POSIX struct posix_dent / struct dirent.
 */
struct brk_dirent {
	__kernel_ino_t d_ino;
	__u32 d_reclen;
	unsigned char d_type;
	char d_name[];
};

#endif
