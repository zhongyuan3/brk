#ifndef UAPI_TIMES_H
#define UAPI_TIMES_H

#include <uapi/types.h>

struct tms {
	__kernel_clock_t tms_utime; /* user time */
	__kernel_clock_t tms_stime; /* system time */
	__kernel_clock_t tms_cutime; /* user time of children */
	__kernel_clock_t tms_cstime; /* system time of children */
};

#endif
