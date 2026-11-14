#ifndef UAPI_SYSCALL_H
#define UAPI_SYSCALL_H

/*
 * BRK kernel system-call ABI.
 *
 * This is the low-level primitive ABI consumed by libc.  POSIX entry points
 * that can be composed from these primitives belong in the C library rather
 * than in this table.  The final entries are BRK-internal interfaces used by
 * libc's signal trampoline and device-control layer.
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
#define SYS_getproc 16
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
#define SYS_sysinfo 32
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
#define SYS_setsid 45
#define SYS_setreuid 46
#define SYS_setregid 47
#define SYS_getgroups 48
#define SYS_setresuid 49
#define SYS_setresgid 50
#define SYS_getpgid 51
#define SYS_getsid 52
#define SYS_sigpending 53
#define SYS_sync 54
#define SYS_clock_settime 55
#define SYS_clock_gettime 56
#define SYS_clock_nanosleep 57
#define SYS_waitid 58
#define SYS_openat 59
#define SYS_mkdirat 60
#define SYS_mknodat 61
#define SYS_fchownat 62
#define SYS_unlinkat 63
#define SYS_renameat 64
#define SYS_linkat 65
#define SYS_symlinkat 66
#define SYS_readlinkat 67
#define SYS_fchmodat 68
#define SYS_faccessat 69
#define SYS_ppoll 70
#define SYS_utimensat 71
#define SYS_accept4 72
#define SYS_dup3 73
#define SYS_pipe2 74
#define SYS_posix_fallocate 75
#define SYS_getdents 76
#define SYS_statfs 77
#define SYS_fstatfs 78
#define SYS_sigtimedwait 79
#define SYS_sigsuspend 80

/* BRK-internal: signal trampoline and device control. */
#define SYS_sigreturn 81
#define SYS_ioctl 82

#endif
