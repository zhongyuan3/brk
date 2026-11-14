#ifndef UAPI_SYSCALL_H
#define UAPI_SYSCALL_H

/*
 * BRK kernel system-call ABI.
 *
 * This is the low-level primitive ABI consumed by libc.  POSIX entry points
 * that can be composed from these primitives belong in the C library rather
 * than in this table.  BRK-internal extensions live in
 * <uapi/syscall_internal.h>.
 */

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_statx 4
#define SYS_poll 7
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_sigaction 12
#define SYS_sigprocmask 13
#define SYS_readv 16
#define SYS_writev 17
#define SYS_select 20
#define SYS_sched_yield 21
#define SYS_msync 22
#define SYS_dup 26
#define SYS_pause 28
#define SYS_getpid 31
#define SYS_socket 32
#define SYS_connect 33
#define SYS_sendto 35
#define SYS_recvfrom 36
#define SYS_shutdown 39
#define SYS_bind 40
#define SYS_listen 41
#define SYS_getsockname 42
#define SYS_getpeername 43
#define SYS_setsockopt 45
#define SYS_getsockopt 46
#define SYS_fork 47
#define SYS_execve 48
#define SYS_exit 49
#define SYS_kill 50
#define SYS_uname 51
#define SYS_fsync 61
#define SYS_fdatasync 62
#define SYS_truncate 63
#define SYS_ftruncate 64
#define SYS_getcwd 65
#define SYS_chdir 66
#define SYS_fchdir 67
#define SYS_rmdir 70
#define SYS_fchmod 77
#define SYS_umask 81
#define SYS_getrusage 83
#define SYS_setuid 87
#define SYS_setgid 88
#define SYS_setpgid 91
#define SYS_getppid 92
#define SYS_setsid 94
#define SYS_setreuid 95
#define SYS_setregid 96
#define SYS_getgroups 97
#define SYS_setresuid 98
#define SYS_getresuid 99
#define SYS_setresgid 100
#define SYS_getresgid 101
#define SYS_getpgid 102
#define SYS_getsid 103
#define SYS_sigpending 104
#define SYS_sync 124
#define SYS_clock_settime 132
#define SYS_clock_gettime 133
#define SYS_clock_getres 134
#define SYS_clock_nanosleep 135
#define SYS_waitid 142
#define SYS_openat 143
#define SYS_mkdirat 144
#define SYS_mknodat 145
#define SYS_fchownat 146
#define SYS_unlinkat 148
#define SYS_renameat 149
#define SYS_linkat 150
#define SYS_symlinkat 151
#define SYS_readlinkat 152
#define SYS_fchmodat 153
#define SYS_faccessat 154
#define SYS_pselect 155
#define SYS_ppoll 156
#define SYS_utimensat 157
#define SYS_accept4 158
#define SYS_dup3 159
#define SYS_pipe2 160
#define SYS_posix_fallocate 161
#define SYS_posix_getdents 162
#define SYS_statvfs 163
#define SYS_fstatvfs 164

#endif
