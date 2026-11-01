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

struct dirent {
	__s32 d_ino;
	__s32 d_off;
	__u16 d_reclen;
	__u8 d_type;
	char d_name[256];
};

struct dirent64 {
	__u64 d_ino;
	__kernel_off_t d_off;
	__u16 d_reclen;
	__u8 d_type;
	char d_name[256];
};

#endif
