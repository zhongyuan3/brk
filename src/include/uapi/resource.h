#ifndef UAPI_RESOURCE_H
#define UAPI_RESOURCE_H

#include <uapi/time.h>

/* getrusage() who values. */
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

/*
 * Kernel-side process time primitive.  All values are nanoseconds, so libc
 * owns the POSIX times()/getrusage() tick and timeval conversions.
 */
struct brk_times {
	__u64 user_ns;
	__u64 system_ns;
	__u64 child_user_ns;
	__u64 child_system_ns;
};

struct rusage {
	struct timeval ru_utime; /* user time used */
	struct timeval ru_stime; /* system time used */
	__kernel_long_t ru_maxrss; /* maximum resident set size */
	__kernel_long_t ru_ixrss; /* integral shared memory size */
	__kernel_long_t ru_idrss; /* integral unshared data size */
	__kernel_long_t ru_isrss; /* integral unshared stack size */
	__kernel_long_t ru_minflt; /* page reclaims */
	__kernel_long_t ru_majflt; /* page faults */
	__kernel_long_t ru_nswap; /* swaps */
	__kernel_long_t ru_inblock; /* block input operations */
	__kernel_long_t ru_oublock; /* block output operations */
	__kernel_long_t ru_msgsnd; /* messages sent */
	__kernel_long_t ru_msgrcv; /* messages received */
	__kernel_long_t ru_nsignals; /* signals received */
	__kernel_long_t ru_nvcsw; /* voluntary context switches */
	__kernel_long_t ru_nivcsw; /* involuntary " */
};

#endif
