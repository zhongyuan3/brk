#ifndef UAPI_SYSCALL_INTERNAL_H
#define UAPI_SYSCALL_INTERNAL_H

/*
 * BRK-internal system call numbers.
 *
 * These interfaces are not part of POSIX.  They are required by the kernel
 * and by the C library implementation (signal trampoline, tty/device control,
 * legacy time wrappers), but must not be used by applications.
 */

#define SYS_sigreturn 191
#define SYS_ioctl 192
#define SYS_gettimeofday 193
#define SYS_settimeofday 194
#define SYS_utime 195

#endif
