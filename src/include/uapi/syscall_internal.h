#ifndef UAPI_SYSCALL_INTERNAL_H
#define UAPI_SYSCALL_INTERNAL_H

/*
 * BRK-internal system call numbers.
 *
 * These interfaces are not part of POSIX.  They are required by the kernel
 * and by the C library implementation (signal trampoline and device control),
 * but must not be used by applications.
 */

#define SYS_sigreturn 85
#define SYS_ioctl 86

#endif
