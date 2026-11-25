#ifndef _UAPI_ASM_SYSCALL_H
#define _UAPI_ASM_SYSCALL_H

/*
 * RISC-V syscall numbers.  The architecture-independent names in
 * <uapi/syscall.h> map to these numbers.  A port with a different
 * numbering scheme only needs to replace this file.
 */

#define __NR_READ 0
#define __NR_WRITE 1
#define __NR_CLOSE 2
#define __NR_SEEK 3
#define __NR_READ_IOV 4
#define __NR_WRITE_IOV 5
#define __NR_DUP 6
#define __NR_DUP_TO 7
#define __NR_PIPE 8
#define __NR_TRUNCATE 9
#define __NR_TRUNCATE_FD 10
#define __NR_PREALLOCATE 11
#define __NR_STAT 12
#define __NR_OPEN 13
#define __NR_MKDIR 14
#define __NR_MKNOD 15
#define __NR_CHOWN 16
#define __NR_UNLINK 17
#define __NR_RENAME 18
#define __NR_LINK 19
#define __NR_SYMLINK 20
#define __NR_READLINK 21
#define __NR_CHMOD 22
#define __NR_ACCESS 23
#define __NR_READ_DIR 24
#define __NR_SET_FILE_TIMES 25
#define __NR_STAT_FS 26
#define __NR_STAT_FS_FD 27
#define __NR_GET_CWD 28
#define __NR_CHDIR 29
#define __NR_CHDIR_FD 30
#define __NR_SET_UMASK 31
#define __NR_SYNC 32
#define __NR_SYNC_FD 33
#define __NR_SYNC_DATA_FD 34
#define __NR_MMAP 35
#define __NR_MPROTECT 36
#define __NR_MUNMAP 37
#define __NR_MSYNC 38
#define __NR_FORK 39
#define __NR_EXEC 40
#define __NR_EXIT 41
#define __NR_EXIT_ALL 42
#define __NR_WAIT 43
#define __NR_GET_PROC 44
#define __NR_CREATE_THREAD 45
#define __NR_FUTEX 46
#define __NR_YIELD 47
#define __NR_GET_SYSINFO 48
#define __NR_GET_TIMES 49
#define __NR_SET_UID_REAL 50
#define __NR_SET_GID_REAL 51
#define __NR_SET_UID_EFFECTIVE 52
#define __NR_SET_GID_EFFECTIVE 53
#define __NR_SET_UID_SAVED 54
#define __NR_SET_GID_SAVED 55
#define __NR_SET_PGID 56
#define __NR_SET_SID 57
#define __NR_GET_PGID 58
#define __NR_GET_SID 59
#define __NR_SEND_SIGNAL 60
#define __NR_SET_SIGNAL 61
#define __NR_SET_SIGMASK 62
#define __NR_GET_PENDING 63
#define __NR_WAIT_SIGNAL 64
#define __NR_GET_CLOCK 65
#define __NR_SET_CLOCK 66
#define __NR_SLEEP_CLOCK 67
#define __NR_CREATE_SOCKET 68
#define __NR_CONNECT 69
#define __NR_BIND 70
#define __NR_LISTEN 71
#define __NR_ACCEPT 72
#define __NR_SEND_TO 73
#define __NR_RECV_FROM 74
#define __NR_SHUTDOWN 75
#define __NR_GET_SOCK_ADDR 76
#define __NR_GET_PEER_ADDR 77
#define __NR_SET_SOCK_OPT 78
#define __NR_GET_SOCK_OPT 79
#define __NR_POLL 80
#define __NR_IOCTL 81
#define __NR_RESTORE_CONTEXT 82

#define __NR_SYSCALL_MAX __NR_RESTORE_CONTEXT

#endif
