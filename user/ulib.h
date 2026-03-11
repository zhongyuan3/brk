#ifndef USER_ULIB_H
#define USER_ULIB_H

#include <aosd/errno.h>
#include <aosd/limits.h>
#include <aosd/printf.h>
#include <aosd/string.h>
#include <aosd/types.h>
#include <uapi/aosd/dirent.h>
#include <uapi/aosd/fcntl.h>
#include <uapi/aosd/resource.h>
#include <uapi/aosd/stat.h>
#include <uapi/aosd/syscall.h>
#include <uapi/aosd/time.h>
#include <uapi/aosd/utsname.h>

/* Standard file descriptors.  */
#define STDIN_FILENO 0 /* Standard input.  */
#define STDOUT_FILENO 1 /* Standard output.  */
#define STDERR_FILENO 2 /* Standard error output.  */

typedef long ssize_t;

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
void exit(int status) __attribute__((noreturn));
int open(const char *path, int flags, ...);
int close(int fd);
int openat(int dirfd, const char *path, int flags, mode_t mode);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t getpid(void);
pid_t getppid(void);
int mkdir(const char *path, mode_t mode);
int mkdirat(int dirfd, const char *path, mode_t mode);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int mknod(const char *path, mode_t mode, dev_t dev);
int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev);
int link(const char *oldpath, const char *newpath);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
	   int flags);
int unlink(const char *path);
int unlinkat(int dirfd, const char *path, int flags);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);
int uname(struct utsname *buf);
int brk(void *addr);
void *sbrk(intptr_t increment);
ssize_t getdents64(int fd, void *dirp, size_t count);
pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);
pid_t fork(void);
int nanosleep(const struct timespec *duration, struct timespec *rem);
clock_t times(struct tms *buf);
int mount(const char *source, const char *target, const char *filesystemtype,
	  unsigned long mountflags, const void *data);
int umount2(const char *target, int flags);
int fstat(int fd, struct stat *buf);
int stat(const char *path, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);

int wait(int *wstatus);
int waitpid(pid_t pid, int *wstatus, int options);

int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execvpe(const char *file, char *const argv[], char *const envp[]);

int dprintf(int fd, char const *fmt, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, char const *fmt, va_list ap);

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

void perror(const char *s);

extern int errno;

#endif
