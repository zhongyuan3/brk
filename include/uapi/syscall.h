#ifndef UAPI_SYSCALL_H
#define UAPI_SYSCALL_H

/*
 * BRK kernel system-call ABI.
 *
 * Naming rules:
 *
 *   1. verb_OBJECT, upper case, words separated by underscores
 *      (GET_PROC, SET_UMASK, CREATE_THREAD).
 *   2. The numbers are the ABI and never change; names may be
 *      reworded, entries may be withdrawn, but a number is never
 *      reused for a different operation.  Numbers of withdrawn
 *      entries stay reserved (15 and 80 below).
 *   3. Only kernel primitives live here: a POSIX entry point that
 *      libc can compose from this table belongs in the C library.
 *   4. The object may be elided when it is the operation's primary
 *      argument (CONNECT, BIND: the socket fd).
 *   5. Names that denote a whole concept are kept whole (FUTEX,
 *      IOCTL, MMAP).
 *   6. _FD marks the fd-based twin of a path operation
 *      (TRUNCATE / TRUNCATE_FD).
 *
 * Path resolution is uniform: every path operation takes a (dirfd,
 * path) pair, with AT_FDCWD for absolute paths.  The old *_AT
 * suffixes are gone because there is no suffix-less twin to tell
 * apart; the pair IS the primitive.
 */

/* ---- file content and descriptors ---- */
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_CLOSE 2
#define SYS_SEEK 4
#define SYS_READ_IOV 10
#define SYS_WRITE_IOV 11
#define SYS_DUP 14
#define SYS_DUP_TO 73
#define SYS_PIPE 74
#define SYS_TRUNCATE 35
#define SYS_TRUNCATE_FD 36
#define SYS_PREALLOCATE 75

/* ---- filesystem paths (dirfd-relative) and metadata ---- */
#define SYS_STAT 3
#define SYS_OPEN 59
#define SYS_MKDIR 60
#define SYS_MKNOD 61
#define SYS_CHOWN 62
#define SYS_UNLINK 63
#define SYS_RENAME 64
#define SYS_LINK 65
#define SYS_SYMLINK 66
#define SYS_READLINK 67
#define SYS_CHMOD 68
#define SYS_ACCESS 69
#define SYS_READ_DIR 76
#define SYS_SET_FILE_TIMES 71
#define SYS_STAT_FS 77
#define SYS_STAT_FS_FD 78
#define SYS_GET_CWD 37
#define SYS_CHDIR 38
#define SYS_CHDIR_FD 39
#define SYS_SET_UMASK 40
#define SYS_SYNC 54
#define SYS_SYNC_FD 33
#define SYS_SYNC_DATA_FD 34

/* 15 reserved: was PAUSE, libc composes it from WAIT_SIGNAL */

/* ---- memory ---- */
#define SYS_MMAP 5
#define SYS_MPROTECT 6
#define SYS_MUNMAP 7
#define SYS_MSYNC 13

/* ---- process ---- */
#define SYS_FORK 28
#define SYS_EXEC 29
#define SYS_EXIT 30
#define SYS_EXIT_ALL 84
#define SYS_WAIT 58
#define SYS_GET_PROC 16
#define SYS_GET_TID 83
#define SYS_CREATE_THREAD 85
#define SYS_FUTEX 86
#define SYS_YIELD 12
#define SYS_GET_SYSINFO 32
#define SYS_GET_TIMES 41

/* ---- identity ----
 * Three uid slots (real, effective, saved) with one setter each;
 * gid mirrors them.  Unprivileged rules: the real id may move to
 * the current real or effective id, the effective id to any of
 * the three, the saved id to the effective one.  libc composes
 * the POSIX setuid/setreuid/setresuid family from these.
 */
#define SYS_SET_UID_REAL 42
#define SYS_SET_GID_REAL 43
#define SYS_SET_UID_EFFECTIVE 46
#define SYS_SET_GID_EFFECTIVE 47
#define SYS_SET_UID_SAVED 49
#define SYS_SET_GID_SAVED 50
#define SYS_SET_PGID 44
#define SYS_SET_SID 45
#define SYS_GET_PGID 51
#define SYS_GET_SID 52
#define SYS_GET_GROUPS 48

/* ---- signals ---- */
#define SYS_SEND_SIGNAL 31
#define SYS_SET_SIGNAL 8
#define SYS_SET_SIGMASK 9
#define SYS_GET_PENDING 53
#define SYS_WAIT_SIGNAL 79

/* WAIT_SIGNAL flags */
#define SIG_WAIT_SETMASK 1	/* install @set as the mask and sleep
				   until a delivery, sigsuspend-style;
				   the caller restores the old mask
				   after the handler returns */

/* 80 reserved: was SIGSUSPEND, folded into WAIT_SIGNAL above */

/* ---- time ---- */
#define SYS_GET_CLOCK 56
#define SYS_SET_CLOCK 55
#define SYS_SLEEP_CLOCK 57

/* ---- sockets ---- */
#define SYS_CREATE_SOCKET 17
#define SYS_CONNECT 18
#define SYS_BIND 22
#define SYS_LISTEN 23
#define SYS_ACCEPT 72
#define SYS_SEND_TO 19
#define SYS_RECV_FROM 20
#define SYS_SHUTDOWN 21
#define SYS_GET_SOCK_ADDR 24
#define SYS_GET_PEER_ADDR 25
#define SYS_SET_SOCK_OPT 26
#define SYS_GET_SOCK_OPT 27

/* ---- events and devices ---- */
#define SYS_POLL 70
#define SYS_IOCTL 82

/* ---- BRK-internal: signal trampoline ---- */
#define SYS_RESTORE_CONTEXT 81

#endif
