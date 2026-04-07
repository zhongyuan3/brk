#ifndef AOSD_TIME_H
#define AOSD_TIME_H

#include <aosd/types.h>

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

struct tms {
	clock_t tms_utime; /* user time */
	clock_t tms_stime; /* system time */
	clock_t tms_cutime; /* user time of children */
	clock_t tms_cstime; /* system time of children */
};

#endif
