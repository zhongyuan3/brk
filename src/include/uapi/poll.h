#ifndef UAPI_POLL_H
#define UAPI_POLL_H

#include <uapi/types.h>

typedef __kernel_ulong_t nfds_t;

struct pollfd {
	__s32 fd;
	__s16 events;
	__s16 revents;
};

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200

#endif
