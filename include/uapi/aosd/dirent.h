#ifndef UAPI_AOSD_DIRENT_H
#define UAPI_AOSD_DIRENT_H

#include <aosd/types.h>

struct dirent64 {
	uint64_t d_ino;
	off_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

#define DIRENT64_NAME_OFFSET offsetof(struct dirent64, d_name)

#endif
