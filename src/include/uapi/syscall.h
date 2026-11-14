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
#define SYS_close 2
#define SYS_statx 3
#define SYS_lseek 4
#define SYS_mmap 5
#define SYS_mprotect 6
#define SYS_munmap 7
#define SYS_sigaction 8
#define SYS_sigprocmask 9
#define SYS_readv 10
#define SYS_writev 11
#define SYS_sched_yield 12
#define SYS_msync 13
#define SYS_dup 14
#define SYS_pause 15
#define SYS_getpid 16
#define SYS_socket 17
#define SYS_connect 18
#define SYS_sendto 19
#define SYS_recvfrom 20
#define SYS_shutdown 21
#define SYS_bind 22
#define SYS_listen 23
#define SYS_getsockname 24
#define SYS_getpeername 25
#define SYS_setsockopt 26
#define SYS_getsockopt 27
#define SYS_fork 28
#define SYS_execve 29
#define SYS_exit 30
#define SYS_kill 31
#define SYS_uname 32
#define SYS_fsync 33
#define SYS_fdatasync 34
#define SYS_truncate 35
#define SYS_ftruncate 36
#define SYS_getcwd 37
#define SYS_chdir 38
#define SYS_fchdir 39
#define SYS_umask 40
#define SYS_get_times 41
#define SYS_setuid 42
#define SYS_setgid 43
#define SYS_setpgid 44
#define SYS_getppid 45
#define SYS_setsid 46
#define SYS_setreuid 47
#define SYS_setregid 48
#define SYS_getgroups 49
#define SYS_setresuid 50
#define SYS_getresuid 51
#define SYS_setresgid 52
#define SYS_getresgid 53
#define SYS_getpgid 54
#define SYS_getsid 55
#define SYS_sigpending 56
#define SYS_sync 57
#define SYS_clock_settime 58
#define SYS_clock_gettime 59
#define SYS_clock_getres 60
#define SYS_clock_nanosleep 61
#define SYS_waitid 62
#define SYS_openat 63
#define SYS_mkdirat 64
#define SYS_mknodat 65
#define SYS_fchownat 66
#define SYS_unlinkat 67
#define SYS_renameat 68
#define SYS_linkat 69
#define SYS_symlinkat 70
#define SYS_readlinkat 71
#define SYS_fchmodat 72
#define SYS_faccessat 73
#define SYS_ppoll 74
#define SYS_utimensat 75
#define SYS_accept4 76
#define SYS_dup3 77
#define SYS_pipe2 78
#define SYS_posix_fallocate 79
#define SYS_getdents 80
#define SYS_statfs 81
#define SYS_fstatfs 82

#endif
