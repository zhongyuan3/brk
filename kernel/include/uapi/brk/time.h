#ifndef UAPI_TIME_H
#define UAPI_TIME_H

#include <uapi/brk/types.h>

typedef long time_t;

struct timeval {
	time_t tv_sec; /* seconds */
	suseconds_t tv_usec; /* microseconds */
};

struct timezone {
	int tz_minuteswest; /* minutes west of Greenwich */
	int tz_dsttime; /* type of dst correction */
};

struct timespec {
	time_t tv_sec;
	long tv_nsec;
};

struct tm {
	int tm_sec; /* Seconds.	[0-60] (1 leap second) */
	int tm_min; /* Minutes.	[0-59] */
	int tm_hour; /* Hours.	[0-23] */
	int tm_mday; /* Day.		[1-31] */
	int tm_mon; /* Month.	[0-11] */
	int tm_year; /* Year	- 1900.  */
	int tm_wday; /* Day of week.	[0-6] */
	int tm_yday; /* Days in year.[0-365]	*/
	int tm_isdst; /* DST.		[-1/0/1]*/
};

#endif
