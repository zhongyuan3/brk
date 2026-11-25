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
 *
 * Each SYS_* entry below is documented with its equivalent C prototype
 * using the __brk_* ABI types.  These are kernel primitives; libc may
 * expose a different POSIX wrapper around the same operation.
 */
#ifndef UAPI_SYSCALL_H
#define UAPI_SYSCALL_H

#include <uapi/asm/syscall.h>

/* __brk_ssize_t sys_read(int fd, char *buf, __brk_size_t count) */
#define SYS_READ __NR_READ

/* __brk_ssize_t sys_write(int fd, const char *buf, __brk_size_t count) */
#define SYS_WRITE __NR_WRITE

/* int sys_close(int fd) */
#define SYS_CLOSE __NR_CLOSE

/* __brk_off_t sys_seek(int fd, __brk_off_t offset, int whence) */
#define SYS_SEEK __NR_SEEK

/* __brk_ssize_t sys_read_iov(int fd, const struct __brk_iovec *iov, int iovcnt) */
#define SYS_READ_IOV __NR_READ_IOV

/* __brk_ssize_t sys_write_iov(int fd, const struct __brk_iovec *iov, int iovcnt) */
#define SYS_WRITE_IOV __NR_WRITE_IOV

/* int sys_dup(int oldfd) */
#define SYS_DUP __NR_DUP

/* int sys_dup_to(int oldfd, int newfd, int flags) */
#define SYS_DUP_TO __NR_DUP_TO

/* int sys_pipe(int *pipefd, int flags) */
#define SYS_PIPE __NR_PIPE

/* int sys_truncate(const char *path, __brk_off_t length) */
#define SYS_TRUNCATE __NR_TRUNCATE

/* int sys_truncate_fd(int fd, __brk_off_t length) */
#define SYS_TRUNCATE_FD __NR_TRUNCATE_FD

/* int sys_preallocate(int fd, int mode, __brk_off_t offset, __brk_off_t len) */
#define SYS_PREALLOCATE __NR_PREALLOCATE

/*
 * int sys_stat(int dirfd, const char *path, int flags,
 *              struct __brk_stat *buf)
 */
#define SYS_STAT __NR_STAT

/* int sys_open(int dirfd, const char *path, int flags, __brk_mode_t mode) */
#define SYS_OPEN __NR_OPEN

/* int sys_mkdir(int dirfd, const char *path, __brk_mode_t mode) */
#define SYS_MKDIR __NR_MKDIR

/*
 * int sys_mknod(int dirfd, const char *path, __brk_mode_t mode,
 *               __brk_dev_t dev)
 */
#define SYS_MKNOD __NR_MKNOD

/*
 * int sys_chown(int dirfd, const char *path, __brk_uid_t uid,
 *               __brk_gid_t gid, int flags)
 */
#define SYS_CHOWN __NR_CHOWN

/* int sys_unlink(int dirfd, const char *path, int flags) */
#define SYS_UNLINK __NR_UNLINK

/*
 * int sys_rename(int olddirfd, const char *oldpath, int newdirfd,
 *                const char *newpath)
 */
#define SYS_RENAME __NR_RENAME

/*
 * int sys_link(int olddirfd, const char *oldpath, int newdirfd,
 *              const char *newpath, int flags)
 */
#define SYS_LINK __NR_LINK

/* int sys_symlink(const char *target, int newdirfd, const char *linkpath) */
#define SYS_SYMLINK __NR_SYMLINK

/*
 * __brk_ssize_t sys_readlink(int dirfd, const char *path, char *buf,
 *                            __brk_size_t bufsiz)
 */
#define SYS_READLINK __NR_READLINK

/* int sys_chmod(int dirfd, const char *path, __brk_mode_t mode, int flags) */
#define SYS_CHMOD __NR_CHMOD

/* int sys_access(int dirfd, const char *path, int mode, int flags) */
#define SYS_ACCESS __NR_ACCESS

/* __brk_ssize_t sys_read_dir(int fd, void *buf, __brk_size_t size, int flags) */
#define SYS_READ_DIR __NR_READ_DIR

/*
 * int sys_set_file_times(int dirfd, const char *path,
 *                        const struct __brk_timespec *times, int flags)
 */
#define SYS_SET_FILE_TIMES __NR_SET_FILE_TIMES

/* int sys_stat_fs(const char *path, struct __brk_statfs *buf) */
#define SYS_STAT_FS __NR_STAT_FS

/* int sys_stat_fs_fd(int fd, struct __brk_statfs *buf) */
#define SYS_STAT_FS_FD __NR_STAT_FS_FD

/* int sys_get_cwd(char *buf, __brk_size_t size) */
#define SYS_GET_CWD __NR_GET_CWD

/* int sys_chdir(const char *path) */
#define SYS_CHDIR __NR_CHDIR

/* int sys_chdir_fd(int fd) */
#define SYS_CHDIR_FD __NR_CHDIR_FD

/* __brk_mode_t sys_set_umask(__brk_mode_t mask) */
#define SYS_SET_UMASK __NR_SET_UMASK

/* void sys_sync(void) */
#define SYS_SYNC __NR_SYNC

/* int sys_sync_fd(int fd) */
#define SYS_SYNC_FD __NR_SYNC_FD

/* int sys_sync_data_fd(int fd) */
#define SYS_SYNC_DATA_FD __NR_SYNC_DATA_FD

/*
 * void *sys_mmap(void *addr, __brk_size_t length, int prot, int flags,
 *                int fd, __brk_off_t offset)
 */
#define SYS_MMAP __NR_MMAP

/* int sys_mprotect(void *addr, __brk_size_t len, int prot) */
#define SYS_MPROTECT __NR_MPROTECT

/* int sys_munmap(void *addr, __brk_size_t len) */
#define SYS_MUNMAP __NR_MUNMAP

/* int sys_msync(void *addr, __brk_size_t len, int flags) */
#define SYS_MSYNC __NR_MSYNC

/* __brk_pid_t sys_fork(void) */
#define SYS_FORK __NR_FORK

/* int sys_exec(const char *path, char *const argv[], char *const envp[]) */
#define SYS_EXEC __NR_EXEC

/* void sys_exit(int status) */
#define SYS_EXIT __NR_EXIT

/* void sys_exit_all(int status) */
#define SYS_EXIT_ALL __NR_EXIT_ALL

/*
 * int sys_wait(int idtype, __brk_pid_t id, __brk_siginfo_t *infop,
 *              int options)
 */
#define SYS_WAIT __NR_WAIT

/* int sys_get_proc(struct __brk_proc *buf) */
#define SYS_GET_PROC __NR_GET_PROC

/* __brk_pid_t sys_create_thread(const struct __brk_thread_create_args *args) */
#define SYS_CREATE_THREAD __NR_CREATE_THREAD

/*
 * int sys_futex(__brk_u32 *uaddr, int op, __brk_u32 val,
 *               const struct __brk_timespec *timeout, __brk_u32 *uaddr2,
 *               __brk_u32 val3)
 */
#define SYS_FUTEX __NR_FUTEX

/* int sys_yield(void) */
#define SYS_YIELD __NR_YIELD

/* int sys_get_sysinfo(struct __brk_sysinfo *buf) */
#define SYS_GET_SYSINFO __NR_GET_SYSINFO

/* int sys_get_times(struct __brk_times *buf) */
#define SYS_GET_TIMES __NR_GET_TIMES

/* int sys_set_uid_real(__brk_uid_t uid) */
#define SYS_SET_UID_REAL __NR_SET_UID_REAL

/* int sys_set_gid_real(__brk_gid_t gid) */
#define SYS_SET_GID_REAL __NR_SET_GID_REAL

/* int sys_set_uid_effective(__brk_uid_t uid) */
#define SYS_SET_UID_EFFECTIVE __NR_SET_UID_EFFECTIVE

/* int sys_set_gid_effective(__brk_gid_t gid) */
#define SYS_SET_GID_EFFECTIVE __NR_SET_GID_EFFECTIVE

/* int sys_set_uid_saved(__brk_uid_t uid) */
#define SYS_SET_UID_SAVED __NR_SET_UID_SAVED

/* int sys_set_gid_saved(__brk_gid_t gid) */
#define SYS_SET_GID_SAVED __NR_SET_GID_SAVED

/* int sys_set_pgid(__brk_pid_t pid, __brk_pid_t pgid) */
#define SYS_SET_PGID __NR_SET_PGID

/* __brk_pid_t sys_set_sid(void) */
#define SYS_SET_SID __NR_SET_SID

/* __brk_pid_t sys_get_pgid(__brk_pid_t pid) */
#define SYS_GET_PGID __NR_GET_PGID

/* __brk_pid_t sys_get_sid(__brk_pid_t pid) */
#define SYS_GET_SID __NR_GET_SID

/* int sys_send_signal(__brk_pid_t pid, int sig) */
#define SYS_SEND_SIGNAL __NR_SEND_SIGNAL

/*
 * int sys_set_signal(int sig, const struct __brk_sigaction *act,
 *                    struct __brk_sigaction *oact)
 */
#define SYS_SET_SIGNAL __NR_SET_SIGNAL

/*
 * int sys_set_sigmask(int how, const __brk_sigset_t *set,
 *                     __brk_sigset_t *oldset)
 */
#define SYS_SET_SIGMASK __NR_SET_SIGMASK

/* int sys_get_pending(__brk_sigset_t *set) */
#define SYS_GET_PENDING __NR_GET_PENDING

/*
 * int sys_wait_signal(const __brk_sigset_t *set,
 *                     const struct __brk_timespec *timeout,
 *                     __brk_siginfo_t *info, int flags)
 */
#define SYS_WAIT_SIGNAL __NR_WAIT_SIGNAL

/* WAIT_SIGNAL flags
 * install @set as the mask and sleep
 * until a delivery, sigsuspend-style;
 * the caller restores the old mask
 * after the handler returns */
#define SIG_WAIT_SETMASK 1

/* int sys_get_clock(int clockid, struct __brk_timespec *tp) */
#define SYS_GET_CLOCK __NR_GET_CLOCK

/* int sys_set_clock(int clockid, const struct __brk_timespec *tp) */
#define SYS_SET_CLOCK __NR_SET_CLOCK

/*
 * int sys_sleep_clock(int clockid, int flags,
 *                     const struct __brk_timespec *req,
 *                     struct __brk_timespec *rem)
 */
#define SYS_SLEEP_CLOCK __NR_SLEEP_CLOCK

/* int sys_create_socket(int family, int type, int protocol) */
#define SYS_CREATE_SOCKET __NR_CREATE_SOCKET

/* int sys_connect(int fd, const void *addr, __brk_socklen_t addrlen) */
#define SYS_CONNECT __NR_CONNECT

/* int sys_bind(int fd, const void *addr, __brk_socklen_t addrlen) */
#define SYS_BIND __NR_BIND

/* int sys_listen(int fd, int backlog) */
#define SYS_LISTEN __NR_LISTEN

/* int sys_accept(int fd, void *addr, __brk_socklen_t *addrlen, int flags) */
#define SYS_ACCEPT __NR_ACCEPT

/*
 * __brk_ssize_t sys_send_to(int fd, const void *buf, __brk_size_t len,
 *                           int flags, const void *addr,
 *                           __brk_socklen_t addrlen)
 */
#define SYS_SEND_TO __NR_SEND_TO

/*
 * __brk_ssize_t sys_recv_from(int fd, void *buf, __brk_size_t len, 
                               int flags,
 *                             void *addr, __brk_socklen_t *addrlen)
 */
#define SYS_RECV_FROM __NR_RECV_FROM

/* int sys_shutdown(int fd, int how) */
#define SYS_SHUTDOWN __NR_SHUTDOWN

/* int sys_get_sock_addr(int fd, void *addr, __brk_socklen_t *addrlen) */
#define SYS_GET_SOCK_ADDR __NR_GET_SOCK_ADDR

/* int sys_get_peer_addr(int fd, void *addr, __brk_socklen_t *addrlen) */
#define SYS_GET_PEER_ADDR __NR_GET_PEER_ADDR

/*
 * int sys_set_sock_opt(int fd, int level, int optname, const void *optval,
 *                      __brk_socklen_t optlen)
 */
#define SYS_SET_SOCK_OPT __NR_SET_SOCK_OPT

/*
 * int sys_get_sock_opt(int fd, int level, int optname, void *optval,
 *                      __brk_socklen_t *optlen)
 */
#define SYS_GET_SOCK_OPT __NR_GET_SOCK_OPT

/*
 * int sys_poll(struct __brk_pollfd *fds, __brk_nfds_t nfds,
 *              const struct __brk_timespec *timeout,
 *              const __brk_sigset_t *sigmask)
 */
#define SYS_POLL __NR_POLL

/* int sys_ioctl(int fd, unsigned int cmd, unsigned long arg) */
#define SYS_IOCTL __NR_IOCTL

/* void sys_restore_context(void) */
#define SYS_RESTORE_CONTEXT __NR_RESTORE_CONTEXT

/* Highest valid syscall number and the size of the dispatch table. */
#define SYS_MAX __NR_SYSCALL_MAX
#define SYS_COUNT (SYS_MAX + 1)

#endif
