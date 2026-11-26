#ifndef _UAPI_TIME_H
#define _UAPI_TIME_H

#include <uapi/types.h>

struct __brk_timeval {
	__brk_time_t tv_sec; /* seconds */
	__brk_suseconds_t tv_usec; /* microseconds */
};

struct __brk_timespec {
	__brk_time_t tv_sec;
	__brk_long_t tv_nsec;
};

/* POSIX clock IDs. */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
/* No suspend support yet, so BOOTTIME is the same as MONOTONIC. */
#define CLOCK_BOOTTIME 7

/* clock_nanosleep() flags. */
#define TIMER_ABSTIME 1

#endif
