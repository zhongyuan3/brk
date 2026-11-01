#ifndef UAPI_TIME_H
#define UAPI_TIME_H

#include <uapi/types.h>

struct timeval {
	__kernel_time_t tv_sec; /* seconds */
	__kernel_suseconds_t tv_usec; /* microseconds */
};

struct timezone {
	__s32 tz_minuteswest; /* minutes west of Greenwich */
	__s32 tz_dsttime; /* type of dst correction */
};

struct timespec {
	__kernel_time_t tv_sec;
	__kernel_long_t tv_nsec;
};

struct utimbuf {
	__kernel_time_t actime;
	__kernel_time_t modtime;
};

struct tm {
	__s32 tm_sec; /* Seconds.	[0-60] (1 leap second) */
	__s32 tm_min; /* Minutes.	[0-59] */
	__s32 tm_hour; /* Hours.	[0-23] */
	__s32 tm_mday; /* Day.		[1-31] */
	__s32 tm_mon; /* Month.	[0-11] */
	__s32 tm_year; /* Year	- 1900.  */
	__s32 tm_wday; /* Day of week.	[0-6] */
	__s32 tm_yday; /* Days in year.[0-365]	*/
	__s32 tm_isdst; /* DST.		[-1/0/1]*/
};

#endif
