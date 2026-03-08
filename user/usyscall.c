#include "internal.h"
#include "ulib.h"

ssize_t read(int fd, void *buf, size_t count)
{
	ssize_t rcnt = syscall(SYS_read, fd, buf, count);
	if (rcnt < 0) {
		errno = -rcnt;
		return -1;
	}
	errno = 0;
	return rcnt;
}

ssize_t write(int fd, const void *buf, size_t count)
{
	ssize_t wcnt = syscall(SYS_write, fd, buf, count);
	if (wcnt < 0) {
		errno = -wcnt;
		return -1;
	}
	errno = 0;
	return wcnt;
}

void exit(int status)
{
	syscall(SYS_exit, status);
	errno = 0;
	while (1) {
	}
}

int open(const char *path, int flags, ...)
{
	int fd = syscall(SYS_open, path, flags, 0);
	if (fd < 0) {
		errno = -fd;
		return -1;
	}
	errno = 0;
	return fd;
}

int openat(int dirfd, const char *path, int flags, mode_t mode)
{
	int fd = syscall(SYS_openat, dirfd, path, flags, mode);
	if (fd < 0) {
		errno = -fd;
		return -1;
	}
	errno = 0;
	return fd;
}

int close(int fd)
{
	int err = syscall(SYS_close, fd);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int dup(int oldfd)
{
	int fd = syscall(SYS_dup, oldfd);
	if (fd < 0) {
		errno = -fd;
		return -1;
	}
	errno = 0;
	return fd;
}

int dup2(int oldfd, int newfd)
{
	int fd = syscall(SYS_dup2, oldfd, newfd);
	if (fd < 0) {
		errno = -fd;
		return -1;
	}
	errno = 0;
	return fd;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
	int err = syscall(SYS_execve, path, argv, envp);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

pid_t getpid(void)
{
	int pid = syscall(SYS_getpid);
	if (pid < 0) {
		errno = -pid;
		return -1;
	}
	errno = 0;
	return pid;
}

pid_t getppid(void)
{
	int ppid = syscall(SYS_getppid);
	if (ppid < 0) {
		errno = -ppid;
		return -1;
	}
	errno = 0;
	return ppid;
}

int mkdir(const char *path, mode_t mode)
{
	int err = syscall(SYS_mkdir, path, mode);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
	int err = syscall(SYS_mkdirat, dirfd, path, mode);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int chdir(const char *path)
{
	int err = syscall(SYS_chdir, path);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

char *getcwd(char *buf, size_t size)
{
	int err = syscall(SYS_getcwd, buf, size);
	if (err) {
		errno = -err;
		return 0;
	}
	errno = 0;
	return buf;
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
	int err = syscall(SYS_mknod, path, mode, dev);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
	int err = syscall(SYS_mknodat, dirfd, path, mode, dev);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int link(const char *oldpath, const char *newpath)
{
	int err = syscall(SYS_link, oldpath, newpath);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
	   int flags)
{
	int err = syscall(SYS_linkat, olddirfd, oldpath, newdirfd, newpath,
			  flags);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int unlink(const char *path)
{
	int err = syscall(SYS_unlink, path);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int unlinkat(int dirfd, const char *path, int flags)
{
	int err = syscall(SYS_unlinkat, dirfd, path, flags);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int pipe(int pipefd[2])
{
	int err = syscall(SYS_pipe, pipefd);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int pipe2(int pipefd[2], int flags)
{
	int err = syscall(SYS_pipe2, pipefd, flags);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int uname(struct utsname *buf)
{
	int err = syscall(SYS_uname, buf);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int brk(void *addr)
{
	if (!addr)
		return 0;
	return syscall(SYS_brk, addr);
}

ssize_t getdents64(int fd, void *dirp, size_t count)
{
	ssize_t rcnt = syscall(SYS_getdents64, fd, dirp, count);
	if (rcnt < 0) {
		errno = -rcnt;
		return -1;
	}
	errno = 0;
	return rcnt;
}

pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage)
{
	pid_t child_pid = syscall(SYS_wait4, pid, wstatus, options, rusage);
	if (child_pid < 0) {
		errno = -child_pid;
		return -1;
	}
	errno = 0;
	return child_pid;
}

pid_t fork(void)
{
	pid_t pid = syscall(SYS_fork);
	if (pid < 0) {
		errno = -pid;
		return -1;
	}
	errno = 0;
	return pid;
}

int nanosleep(const struct timespec *duration, struct timespec *rem)
{
	int err = syscall(SYS_nanosleep, duration, rem);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

clock_t times(struct tms *buf)
{
	clock_t t = syscall(SYS_times, buf);
	if (t < 0) {
		errno = -t;
		return -1;
	}
	errno = 0;
	return t;
}

int mount(const char *source, const char *target, const char *filesystemtype,
	  unsigned long mountflags, const void *data)
{
	int err = syscall(SYS_mount, source, target, filesystemtype, mountflags,
			  data);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int umount2(const char *target, int flags)
{
	int err = syscall(SYS_umount2, target, flags);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
	int err = syscall(SYS_gettimeofday, tv, tz);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
	int err = syscall(SYS_settimeofday, tv, tz);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int fstat(int fd, struct stat *buf)
{
	int err = syscall(SYS_fstat, fd, buf);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int stat(const char *path, struct stat *buf)
{
	int err = syscall(SYS_stat, path, buf);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}

int lstat(const char *path, struct stat *buf)
{
	int err = syscall(SYS_lstat, path, buf);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return 0;
}
