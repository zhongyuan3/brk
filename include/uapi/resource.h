#ifndef _UAPI_RESOURCE_H
#define _UAPI_RESOURCE_H

#include <uapi/time.h>

/* getrusage() who values. */
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

/*
 * Kernel-side process time primitive.  All values are nanoseconds, so libc
 * owns the POSIX times()/getrusage() tick and timeval conversions.
 */
struct __brk_times {
	__brk_u64 user_ns;
	__brk_u64 system_ns;
	__brk_u64 child_user_ns;
	__brk_u64 child_system_ns;
};

struct __brk_rusage {
	struct __brk_timeval ru_utime; /* user time used */
	struct __brk_timeval ru_stime; /* system time used */
	__brk_long_t ru_maxrss; /* maximum resident set size */
	__brk_long_t ru_ixrss; /* integral shared memory size */
	__brk_long_t ru_idrss; /* integral unshared data size */
	__brk_long_t ru_isrss; /* integral unshared stack size */
	__brk_long_t ru_minflt; /* page reclaims */
	__brk_long_t ru_majflt; /* page faults */
	__brk_long_t ru_nswap; /* swaps */
	__brk_long_t ru_inblock; /* block input operations */
	__brk_long_t ru_oublock; /* block output operations */
	__brk_long_t ru_msgsnd; /* messages sent */
	__brk_long_t ru_msgrcv; /* messages received */
	__brk_long_t ru_nsignals; /* signals received */
	__brk_long_t ru_nvcsw; /* voluntary context switches */
	__brk_long_t ru_nivcsw; /* involuntary " */
};

#endif
