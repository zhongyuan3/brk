#ifndef UAPI_CAPABILITY_H
#define UAPI_CAPABILITY_H

#include <uapi/types.h>

#define _LINUX_CAPABILITY_VERSION_3 0x20080522

struct cap_header {
	__u32 version;
	__s32 pid;
};

struct cap_data {
	__u32 effective;
	__u32 permitted;
	__u32 inheritable;
};

#define CAP_CHOWN 0
#define CAP_DAC_OVERRIDE 1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER 3
#define CAP_FSETID 4
#define CAP_KILL 5
#define CAP_SETGID 6
#define CAP_SETUID 7
#define CAP_SETPCAP 8
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_ADMIN 12
#define CAP_NET_RAW 13
#define CAP_SYS_CHROOT 18
#define CAP_SYS_ADMIN 21
#define CAP_SYS_TIME 25
#define CAP_MKNOD 27
#define CAP_SETFCAP 31

#define CAP_LAST_CAP 31
#define CAP_FULL_SET 0xffffffffULL

#endif
