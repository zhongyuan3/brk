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

/* __brk_ssize_t sys_read(int fd, char *buf, __brk_size_t count) */
#define SYS_READ 0

/* __brk_ssize_t sys_write(int fd, const char *buf, __brk_size_t count) */
#define SYS_WRITE 1

/* int sys_close(int fd) */
#define SYS_CLOSE 2

/* __brk_off_t sys_seek(int fd, __brk_off_t offset, int whence) */
#define SYS_SEEK 3

/* __brk_ssize_t sys_read_iov(int fd, const struct __brk_iovec *iov, int iovcnt) */
#define SYS_READ_IOV 4

/* __brk_ssize_t sys_write_iov(int fd, const struct __brk_iovec *iov, int iovcnt) */
#define SYS_WRITE_IOV 5

/* int sys_dup(int oldfd) */
#define SYS_DUP 6

/* int sys_dup_to(int oldfd, int newfd, int flags) */
#define SYS_DUP_TO 7

/* int sys_pipe(int *pipefd, int flags) */
#define SYS_PIPE 8

/* int sys_truncate(const char *path, __brk_off_t length) */
#define SYS_TRUNCATE 9

/* int sys_truncate_fd(int fd, __brk_off_t length) */
#define SYS_TRUNCATE_FD 10

/* int sys_preallocate(int fd, int mode, __brk_off_t offset, __brk_off_t len) */
#define SYS_PREALLOCATE 11

/*
 * int sys_stat(int dirfd, const char *path, int flags,
 *              struct __brk_stat *buf)
 */
#define SYS_STAT 12

/* int sys_open(int dirfd, const char *path, int flags, __brk_mode_t mode) */
#define SYS_OPEN 13

/* int sys_mkdir(int dirfd, const char *path, __brk_mode_t mode) */
#define SYS_MKDIR 14

/*
 * int sys_mknod(int dirfd, const char *path, __brk_mode_t mode,
 *               __brk_dev_t dev)
 */
#define SYS_MKNOD 15

/*
 * int sys_chown(int dirfd, const char *path, __brk_uid_t uid,
 *               __brk_gid_t gid, int flags)
 */
#define SYS_CHOWN 16

/* int sys_unlink(int dirfd, const char *path, int flags) */
#define SYS_UNLINK 17

/*
 * int sys_rename(int olddirfd, const char *oldpath, int newdirfd,
 *                const char *newpath)
 */
#define SYS_RENAME 18

/*
 * int sys_link(int olddirfd, const char *oldpath, int newdirfd,
 *              const char *newpath, int flags)
 */
#define SYS_LINK 19

/* int sys_symlink(const char *target, int newdirfd, const char *linkpath) */
#define SYS_SYMLINK 20

/*
 * __brk_ssize_t sys_readlink(int dirfd, const char *path, char *buf,
 *                            __brk_size_t bufsiz)
 */
#define SYS_READLINK 21

/* int sys_chmod(int dirfd, const char *path, __brk_mode_t mode, int flags) */
#define SYS_CHMOD 22

/* int sys_access(int dirfd, const char *path, int mode, int flags) */
#define SYS_ACCESS 23

/* __brk_ssize_t sys_read_dir(int fd, void *buf, __brk_size_t size, int flags) */
#define SYS_READ_DIR 24

/*
 * int sys_set_file_times(int dirfd, const char *path,
 *                        const struct __brk_timespec *times, int flags)
 */
#define SYS_SET_FILE_TIMES 25

/* int sys_stat_fs(const char *path, struct __brk_statfs *buf) */
#define SYS_STAT_FS 26

/* int sys_stat_fs_fd(int fd, struct __brk_statfs *buf) */
#define SYS_STAT_FS_FD 27

/* int sys_get_cwd(char *buf, __brk_size_t size) */
#define SYS_GET_CWD 28

/* int sys_chdir(const char *path) */
#define SYS_CHDIR 29

/* int sys_chdir_fd(int fd) */
#define SYS_CHDIR_FD 30

/* __brk_mode_t sys_set_umask(__brk_mode_t mask) */
#define SYS_SET_UMASK 31

/* void sys_sync(void) */
#define SYS_SYNC 32

/* int sys_sync_fd(int fd) */
#define SYS_SYNC_FD 33

/* int sys_sync_data_fd(int fd) */
#define SYS_SYNC_DATA_FD 34

/*
 * void *sys_mmap(void *addr, __brk_size_t length, int prot, int flags,
 *                int fd, __brk_off_t offset)
 */
#define SYS_MMAP 35

/* int sys_mprotect(void *addr, __brk_size_t len, int prot) */
#define SYS_MPROTECT 36

/* int sys_munmap(void *addr, __brk_size_t len) */
#define SYS_MUNMAP 37

/* int sys_msync(void *addr, __brk_size_t len, int flags) */
#define SYS_MSYNC 38

/* __brk_pid_t sys_fork(void) */
#define SYS_FORK 39

/* int sys_exec(const char *path, char *const argv[], char *const envp[]) */
#define SYS_EXEC 40

/* void sys_exit(int status) */
#define SYS_EXIT 41

/* void sys_exit_all(int status) */
#define SYS_EXIT_ALL 42

/*
 * int sys_wait(int idtype, __brk_pid_t id, __brk_siginfo_t *infop,
 *              int options)
 */
#define SYS_WAIT 43

/* int sys_get_proc(struct __brk_proc *buf) */
#define SYS_GET_PROC 44

/* __brk_pid_t sys_create_thread(const struct __brk_thread_create_args *args) */
#define SYS_CREATE_THREAD 45

/*
 * int sys_futex(__brk_u32 *uaddr, int op, __brk_u32 val,
 *               const struct __brk_timespec *timeout, __brk_u32 *uaddr2,
 *               __brk_u32 val3)
 */
#define SYS_FUTEX 46

/* int sys_yield(void) */
#define SYS_YIELD 47

/* int sys_get_sysinfo(struct __brk_sysinfo *buf) */
#define SYS_GET_SYSINFO 48

/* int sys_get_times(struct __brk_times *buf) */
#define SYS_GET_TIMES 49

/* int sys_set_uid_real(__brk_uid_t uid) */
#define SYS_SET_UID_REAL 50

/* int sys_set_gid_real(__brk_gid_t gid) */
#define SYS_SET_GID_REAL 51

/* int sys_set_uid_effective(__brk_uid_t uid) */
#define SYS_SET_UID_EFFECTIVE 52

/* int sys_set_gid_effective(__brk_gid_t gid) */
#define SYS_SET_GID_EFFECTIVE 53

/* int sys_set_uid_saved(__brk_uid_t uid) */
#define SYS_SET_UID_SAVED 54

/* int sys_set_gid_saved(__brk_gid_t gid) */
#define SYS_SET_GID_SAVED 55

/* int sys_set_pgid(__brk_pid_t pid, __brk_pid_t pgid) */
#define SYS_SET_PGID 56

/* __brk_pid_t sys_set_sid(void) */
#define SYS_SET_SID 57

/* __brk_pid_t sys_get_pgid(__brk_pid_t pid) */
#define SYS_GET_PGID 58

/* __brk_pid_t sys_get_sid(__brk_pid_t pid) */
#define SYS_GET_SID 59

/* int sys_send_signal(__brk_pid_t pid, int sig) */
#define SYS_SEND_SIGNAL 60

/*
 * int sys_set_signal(int sig, const struct __brk_sigaction *act,
 *                    struct __brk_sigaction *oact)
 */
#define SYS_SET_SIGNAL 61

/*
 * int sys_set_sigmask(int how, const __brk_sigset_t *set,
 *                     __brk_sigset_t *oldset)
 */
#define SYS_SET_SIGMASK 62

/* int sys_get_pending(__brk_sigset_t *set) */
#define SYS_GET_PENDING 63

/*
 * int sys_wait_signal(const __brk_sigset_t *set,
 *                     const struct __brk_timespec *timeout,
 *                     __brk_siginfo_t *info, int flags)
 */
#define SYS_WAIT_SIGNAL 64

/* WAIT_SIGNAL flags
 * install @set as the mask and sleep
 * until a delivery, sigsuspend-style;
 * the caller restores the old mask
 * after the handler returns */
#define SIG_WAIT_SETMASK 1

/* int sys_get_clock(int clockid, struct __brk_timespec *tp) */
#define SYS_GET_CLOCK 65

/* int sys_set_clock(int clockid, const struct __brk_timespec *tp) */
#define SYS_SET_CLOCK 66

/*
 * int sys_sleep_clock(int clockid, int flags,
 *                     const struct __brk_timespec *req,
 *                     struct __brk_timespec *rem)
 */
#define SYS_SLEEP_CLOCK 67

/* int sys_create_socket(int family, int type, int protocol) */
#define SYS_CREATE_SOCKET 68

/* int sys_connect(int fd, const void *addr, __brk_socklen_t addrlen) */
#define SYS_CONNECT 69

/* int sys_bind(int fd, const void *addr, __brk_socklen_t addrlen) */
#define SYS_BIND 70

/* int sys_listen(int fd, int backlog) */
#define SYS_LISTEN 71

/* int sys_accept(int fd, void *addr, __brk_socklen_t *addrlen, int flags) */
#define SYS_ACCEPT 72

/*
 * __brk_ssize_t sys_send_to(int fd, const void *buf, __brk_size_t len,
 *                           int flags, const void *addr,
 *                           __brk_socklen_t addrlen)
 */
#define SYS_SEND_TO 73

/*
 * __brk_ssize_t sys_recv_from(int fd, void *buf, __brk_size_t len, 
                               int flags,
 *                             void *addr, __brk_socklen_t *addrlen)
 */
#define SYS_RECV_FROM 74

/* int sys_shutdown(int fd, int how) */
#define SYS_SHUTDOWN 75

/* int sys_get_sock_addr(int fd, void *addr, __brk_socklen_t *addrlen) */
#define SYS_GET_SOCK_ADDR 76

/* int sys_get_peer_addr(int fd, void *addr, __brk_socklen_t *addrlen) */
#define SYS_GET_PEER_ADDR 77

/*
 * int sys_set_sock_opt(int fd, int level, int optname, const void *optval,
 *                      __brk_socklen_t optlen)
 */
#define SYS_SET_SOCK_OPT 78

/*
 * int sys_get_sock_opt(int fd, int level, int optname, void *optval,
 *                      __brk_socklen_t *optlen)
 */
#define SYS_GET_SOCK_OPT 79

/*
 * int sys_poll(struct __brk_pollfd *fds, __brk_nfds_t nfds,
 *              const struct __brk_timespec *timeout,
 *              const __brk_sigset_t *sigmask)
 */
#define SYS_POLL 80

/* int sys_ioctl(int fd, unsigned int cmd, unsigned long arg) */
#define SYS_IOCTL 81

/* void sys_restore_context(void) */
#define SYS_RESTORE_CONTEXT 82

#endif
