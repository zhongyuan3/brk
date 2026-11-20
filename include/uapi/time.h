#ifndef UAPI_TIME_H
#define UAPI_TIME_H

#include <uapi/types.h>

struct timeval {
	__brk_time_t tv_sec; /* seconds */
	__brk_suseconds_t tv_usec; /* microseconds */
};

struct timezone {
	__brk_s32 tz_minuteswest; /* minutes west of Greenwich */
	__brk_s32 tz_dsttime; /* type of dst correction */
};

struct timespec {
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

struct utimbuf {
	__brk_time_t actime;
	__brk_time_t modtime;
};

struct tm {
	__brk_s32 tm_sec; /* Seconds.	[0-60] (1 leap second) */
	__brk_s32 tm_min; /* Minutes.	[0-59] */
	__brk_s32 tm_hour; /* Hours.	[0-23] */
	__brk_s32 tm_mday; /* Day.		[1-31] */
	__brk_s32 tm_mon; /* Month.	[0-11] */
	__brk_s32 tm_year; /* Year	- 1900.  */
	__brk_s32 tm_wday; /* Day of week.	[0-6] */
	__brk_s32 tm_yday; /* Days in year.[0-365]	*/
	__brk_s32 tm_isdst; /* DST.		[-1/0/1]*/
};

#endif
