#ifndef UAPI_PROC_H
#define UAPI_PROC_H

#include <uapi/types.h>

/*
 * Kernel-side identity snapshot.  libc derives the POSIX getpid/getuid/
 * getresuid/getppid family from this structure.
 */
struct brk_proc {
	__brk_pid_t pid;
	__brk_pid_t ppid;
	__brk_pid_t pgid;
	__brk_pid_t sid;
	__brk_pid_t tid;
	__brk_uid_t uid;
	__brk_uid_t euid;
	__brk_uid_t suid;
	__brk_gid_t gid;
	__brk_gid_t egid;
	__brk_gid_t sgid;

	/* Supplementary group ids, cred.ngroups of them valid. */
#define BRK_NGROUPS_MAX 32
	int ngroups;
	__brk_gid_t groups[BRK_NGROUPS_MAX];
};

/*
 * Native thread creation ABI.  The new thread starts at @entry with
 * a0 = @arg and sp = @stack.  @tls is written to tp; zero inherits the
 * creator's tp.  @clear_tid, when non-NULL, is cleared to zero on exit.
 */
struct brk_thread_create_args {
	__brk_ulong_t stack;
	__brk_ulong_t entry;
	__brk_ulong_t arg;
	__brk_ulong_t tls;
	__brk_ulong_t clear_tid;
	__brk_ulong_t flags; /* reserved, must be zero */
};

#endif
