#ifndef UAPI_PROC_H
#define UAPI_PROC_H

#include <uapi/types.h>

/*
 * Kernel-side identity snapshot.  libc derives the POSIX getpid/getuid/
 * getresuid/getppid family from this structure.
 */
struct brk_proc {
	__kernel_pid_t pid;
	__kernel_pid_t ppid;
	__kernel_pid_t pgid;
	__kernel_pid_t sid;
	__kernel_uid_t uid;
	__kernel_uid_t euid;
	__kernel_uid_t suid;
	__kernel_gid_t gid;
	__kernel_gid_t egid;
	__kernel_gid_t sgid;
};

#endif
