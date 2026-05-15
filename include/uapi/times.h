#ifndef UAPI_BRK_TIMES_H
#define UAPI_BRK_TIMES_H

#include <uapi/types.h>

struct tms {
	clock_t tms_utime; /* user time */
	clock_t tms_stime; /* system time */
	clock_t tms_cutime; /* user time of children */
	clock_t tms_cstime; /* system time of children */
};

#endif
