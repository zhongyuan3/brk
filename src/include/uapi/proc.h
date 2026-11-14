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

/*
 * Native thread creation ABI.  The new thread starts at @entry with
 * a0 = @arg and sp = @stack.  @tls is written to tp; zero inherits the
 * creator's tp.  @clear_tid, when non-NULL, is cleared to zero on exit.
 */
struct brk_thread_create_args {
	__kernel_ulong_t stack;
	__kernel_ulong_t entry;
	__kernel_ulong_t arg;
	__kernel_ulong_t tls;
	__kernel_ulong_t clear_tid;
	__kernel_ulong_t flags; /* reserved, must be zero */
};

#endif
