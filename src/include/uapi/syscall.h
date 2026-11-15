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

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_CLOSE 2
#define SYS_STATX 3
#define SYS_LSEEK 4
#define SYS_MMAP 5
#define SYS_MPROTECT 6
#define SYS_MUNMAP 7
#define SYS_SIGACTION 8
#define SYS_SIGPROCMASK 9
#define SYS_READV 10
#define SYS_WRITEV 11
#define SYS_SCHED_YIELD 12
#define SYS_MSYNC 13
#define SYS_DUP 14
#define SYS_PAUSE 15
#define SYS_GETPROC 16
#define SYS_SOCKET 17
#define SYS_CONNECT 18
#define SYS_SENDTO 19
#define SYS_RECVFROM 20
#define SYS_SHUTDOWN 21
#define SYS_BIND 22
#define SYS_LISTEN 23
#define SYS_GETSOCKNAME 24
#define SYS_GETPEERNAME 25
#define SYS_SETSOCKOPT 26
#define SYS_GETSOCKOPT 27
#define SYS_FORK 28
#define SYS_EXECVE 29
#define SYS_EXIT 30
#define SYS_KILL 31
#define SYS_SYSINFO 32
#define SYS_FSYNC 33
#define SYS_FDATASYNC 34
#define SYS_TRUNCATE 35
#define SYS_FTRUNCATE 36
#define SYS_GETCWD 37
#define SYS_CHDIR 38
#define SYS_FCHDIR 39
#define SYS_UMASK 40
#define SYS_GET_TIMES 41
#define SYS_SETUID 42
#define SYS_SETGID 43
#define SYS_SETPGID 44
#define SYS_SETSID 45
#define SYS_SETREUID 46
#define SYS_SETREGID 47
#define SYS_GETGROUPS 48
#define SYS_SETRESUID 49
#define SYS_SETRESGID 50
#define SYS_GETPGID 51
#define SYS_GETSID 52
#define SYS_SIGPENDING 53
#define SYS_SYNC 54
#define SYS_CLOCK_SETTIME 55
#define SYS_CLOCK_GETTIME 56
#define SYS_CLOCK_NANOSLEEP 57
#define SYS_WAITID 58
#define SYS_OPENAT 59
#define SYS_MKDIRAT 60
#define SYS_MKNODAT 61
#define SYS_FCHOWNAT 62
#define SYS_UNLINKAT 63
#define SYS_RENAMEAT 64
#define SYS_LINKAT 65
#define SYS_SYMLINKAT 66
#define SYS_READLINKAT 67
#define SYS_FCHMODAT 68
#define SYS_FACCESSAT 69
#define SYS_PPOLL 70
#define SYS_UTIMENSAT 71
#define SYS_ACCEPT4 72
#define SYS_DUP3 73
#define SYS_PIPE2 74
#define SYS_FALLOCATE 75
#define SYS_GETDENTS 76
#define SYS_STATFS 77
#define SYS_FSTATFS 78
#define SYS_SIGTIMEDWAIT 79
#define SYS_SIGSUSPEND 80

/* BRK-internal: signal trampoline and device control. */
#define SYS_SIGRETURN 81
#define SYS_IOCTL 82

/* Process, thread and futex primitives. */
#define SYS_GETTID 83
#define SYS_EXIT_GROUP 84
#define SYS_THREAD_CREATE 85
#define SYS_FUTEX 86

#endif
