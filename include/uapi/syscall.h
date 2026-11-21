#ifndef UAPI_SYSCALL_H
#define UAPI_SYSCALL_H

/*
 * BRK kernel system-call ABI.
 *
 * Naming rules:
 *
 *   1. verb_OBJECT, upper case, words separated by underscores
 *      (GET_PROC, SET_UMASK, CREATE_THREAD).
 *   2. Numbers follow the table order: entries are grouped by
 *      subsystem and numbered densely from 0.  When an entry is
 *      withdrawn, the table is repacked.  Kernel, libc and the
 *      on-target compiler are always built together from this
 *      tree, so names are the interface; numbers are internal.
 *      Nothing outside this tree may hard-code a number.
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
#define SYS_SEEK 3
#define SYS_READ_IOV 4
#define SYS_WRITE_IOV 5
#define SYS_DUP 6
#define SYS_DUP_TO 7
#define SYS_PIPE 8
#define SYS_TRUNCATE 9
#define SYS_TRUNCATE_FD 10
#define SYS_PREALLOCATE 11

/* ---- filesystem paths (dirfd-relative) and metadata ---- */
#define SYS_STAT 12
#define SYS_OPEN 13
#define SYS_MKDIR 14
#define SYS_MKNOD 15
#define SYS_CHOWN 16
#define SYS_UNLINK 17
#define SYS_RENAME 18
#define SYS_LINK 19
#define SYS_SYMLINK 20
#define SYS_READLINK 21
#define SYS_CHMOD 22
#define SYS_ACCESS 23
#define SYS_READ_DIR 24
#define SYS_SET_FILE_TIMES 25
#define SYS_STAT_FS 26
#define SYS_STAT_FS_FD 27
#define SYS_GET_CWD 28
#define SYS_CHDIR 29
#define SYS_CHDIR_FD 30
#define SYS_SET_UMASK 31
#define SYS_SYNC 32
#define SYS_SYNC_FD 33
#define SYS_SYNC_DATA_FD 34

/* ---- memory ---- */
#define SYS_MMAP 35
#define SYS_MPROTECT 36
#define SYS_MUNMAP 37
#define SYS_MSYNC 38

/* ---- process ---- */
#define SYS_FORK 39
#define SYS_EXEC 40
#define SYS_EXIT 41
#define SYS_EXIT_ALL 42
#define SYS_WAIT 43
#define SYS_GET_PROC 44
#define SYS_CREATE_THREAD 45
#define SYS_FUTEX 46
#define SYS_YIELD 47
#define SYS_GET_SYSINFO 48
#define SYS_GET_TIMES 49

/* ---- identity ----
 * Three uid slots (real, effective, saved) with one setter each;
 * gid mirrors them.  Unprivileged rule: a slot may move to any of
 * the process's three current ids of that kind.  libc composes
 * the POSIX setuid/setreuid/setresuid family from these, and the
 * whole get*id family from GET_PROC.
 */
#define SYS_SET_UID_REAL 50
#define SYS_SET_GID_REAL 51
#define SYS_SET_UID_EFFECTIVE 52
#define SYS_SET_GID_EFFECTIVE 53
#define SYS_SET_UID_SAVED 54
#define SYS_SET_GID_SAVED 55
#define SYS_SET_PGID 56
#define SYS_SET_SID 57
#define SYS_GET_PGID 58
#define SYS_GET_SID 59

/* ---- signals ---- */
#define SYS_SEND_SIGNAL 60
#define SYS_SET_SIGNAL 61
#define SYS_SET_SIGMASK 62
#define SYS_GET_PENDING 63
#define SYS_WAIT_SIGNAL 64

/* WAIT_SIGNAL flags */
#define SIG_WAIT_SETMASK 1	/* install @set as the mask and sleep
				   until a delivery, sigsuspend-style;
				   the caller restores the old mask
				   after the handler returns */

/* ---- time ---- */
#define SYS_GET_CLOCK 65
#define SYS_SET_CLOCK 66
#define SYS_SLEEP_CLOCK 67

/* ---- sockets ---- */
#define SYS_CREATE_SOCKET 68
#define SYS_CONNECT 69
#define SYS_BIND 70
#define SYS_LISTEN 71
#define SYS_ACCEPT 72
#define SYS_SEND_TO 73
#define SYS_RECV_FROM 74
#define SYS_SHUTDOWN 75
#define SYS_GET_SOCK_ADDR 76
#define SYS_GET_PEER_ADDR 77
#define SYS_SET_SOCK_OPT 78
#define SYS_GET_SOCK_OPT 79

/* ---- events and devices ---- */
#define SYS_POLL 80
#define SYS_IOCTL 81

/* ---- BRK-internal: signal trampoline ---- */
#define SYS_RESTORE_CONTEXT 82

#endif
