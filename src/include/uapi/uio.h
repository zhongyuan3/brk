#ifndef UAPI_UIO_H
#define UAPI_UIO_H

#include <uapi/types.h>

#define IOV_MAX 1024

struct iovec {
	void *iov_base;
	__kernel_size_t iov_len;
};

#endif
