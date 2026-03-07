#include "ulib.h"
#include "internal.h"

ssize_t read(int fd, void *buf, size_t count)
{
	return syscall(SYS_READ, fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return syscall(SYS_WRITE, fd, buf, count);
}

void exit(int status)
{
	syscall(SYS_EXIT, status);
	while (1) {
	}
}

int open(const char *path, int flags, ...)
{
	return syscall(SYS_OPEN, path, flags, 0);
}

int openat(int dirfd, const char *path, int flags, mode_t mode)
{
	return syscall(SYS_OPENAT, dirfd, path, flags, mode);
}

int close(int fd)
{
	return syscall(SYS_CLOSE, fd);
}

int dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	int ret;
	va_start(ap, fmt);
	ret = vdprintf(fd, fmt, ap);
	va_end(ap);
	return ret;
}

static int dis_write(struct display *dis, char const *buf, size_t len,
		     size_t *wlen)
{
	int *fd = dis->priv;
	ssize_t n = write(*fd, buf, len);
	if (n < 0)
		return n;
	if (wlen)
		*wlen = n;
	return 0;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
	struct display dis = {
		.write = dis_write,
		.priv = &fd,
	};
	return printf_core(&dis, fmt, ap);
}

void _start(int argc, char **argv, char **envp)
{
	extern int main(int argc, char **argv, char **envp);
	exit(main(argc, argv, envp));
}

int dup(int oldfd)
{
	return syscall(SYS_DUP, oldfd);
}

int dup2(int oldfd, int newfd)
{
	return syscall(SYS_DUP2, oldfd, newfd);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
	return syscall(SYS_EXECVE, path, argv, envp);
}

pid_t getpid(void)
{
	return syscall(SYS_GETPID);
}

pid_t getppid(void)
{
	return syscall(SYS_GETPPID);
}

int mkdir(const char *path, mode_t mode)
{
	return syscall(SYS_MKDIR, path, mode);
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
	return syscall(SYS_MKDIRAT, dirfd, path, mode);
}

int chdir(const char *path)
{
	return syscall(SYS_CHDIR, path);
}

char *getcwd(char *buf, size_t size)
{
	int err = syscall(SYS_GETCWD, buf, size);
	if (err)
		return NULL;
	return buf;
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
	return syscall(SYS_MKNOD, path, mode, dev);
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
	return syscall(SYS_MKNODAT, dirfd, path, mode, dev);
}

int link(const char *oldpath, const char *newpath)
{
	return syscall(SYS_LINK, oldpath, newpath);
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
	   int flags)
{
	return syscall(SYS_LINKAT, olddirfd, oldpath, newdirfd, newpath, flags);
}

int unlink(const char *path)
{
	return syscall(SYS_UNLINK, path);
}

int unlinkat(int dirfd, const char *path, int flags)
{
	return syscall(SYS_UNLINKAT, dirfd, path, flags);
}

int pipe(int pipefd[2])
{
	return syscall(SYS_PIPE, pipefd);
}

int pipe2(int pipefd[2], int flags)
{
	return syscall(SYS_PIPE2, pipefd, flags);
}

int uname(struct utsname *buf)
{
	return syscall(SYS_UNAME, buf);
}

int brk(void *addr)
{
	return syscall(SYS_BRK, addr);
}

void *sbrk(intptr_t increment)
{
	return (void *)syscall(SYS_SBRK, increment);
}

ssize_t getdents64(int fd, void *dirp, size_t count)
{
	return syscall(SYS_GETDENTS64, fd, dirp, count);
}

pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage)
{
	return syscall(SYS_WAIT4, pid, wstatus, options, rusage);
}

pid_t fork(void)
{
	return syscall(SYS_FORK);
}

int nanosleep(const struct timespec *duration, struct timespec *rem)
{
	return syscall(SYS_NANOSLEEP, duration, rem);
}

clock_t times(struct tms *buf)
{
	return syscall(SYS_TIMES, buf);
}

int mount(const char *source, const char *target, const char *filesystemtype,
	  unsigned long mountflags, const void *data)
{
	return syscall(SYS_MOUNT, source, target, filesystemtype, mountflags,
		       data);
}

int umount2(const char *target, int flags)
{
	return syscall(SYS_UMOUNT2, target, flags);
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
	return syscall(SYS_GETTIMEOFDAY, tv, tz);
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
	return syscall(SYS_SETTIMEOFDAY, tv, tz);
}

int fstat(int fd, struct stat *buf)
{
	return syscall(SYS_FSTAT, fd, buf);
}

int stat(const char *path, struct stat *buf)
{
	return syscall(SYS_STAT, path, buf);
}

int lstat(const char *path, struct stat *buf)
{
	return syscall(SYS_LSTAT, path, buf);
}

struct block {
	struct block *next;
	size_t size;
	bool free;
};

static struct block *head = NULL;

void *malloc(size_t size)
{
	struct block *curr, *prev, *new_block;
	size_t tot_size;

	if (size <= 0)
		return NULL;

	curr = head;
	prev = NULL;

	while (curr) {
		if (curr->free && curr->size >= size) {
			curr->free = false;
			return curr + 1;
		}
		prev = curr;
		curr = curr->next;
	}

	tot_size = sizeof(struct block) + size;
	new_block = sbrk(tot_size);
	if (new_block == (void *)(-1))
		return NULL;

	new_block->size = size;
	new_block->free = false;
	new_block->next = NULL;

	if (!prev)
		head = new_block;
	else
		prev->next = new_block;

	return new_block + 1;
}

void *calloc(size_t nmemb, size_t size)
{
	void *ptr = malloc(nmemb * size);
	if (ptr)
		memset(ptr, 0, nmemb * size);
	return ptr;
}

void *realloc(void *ptr, size_t size)
{
	struct block *blk;
	void *new_ptr;

	new_ptr = malloc(size);
	if (!new_ptr)
		return NULL;

	blk = ((struct block *)(ptr)) - 1;
	memcpy(new_ptr, ptr, blk->size);
	free(ptr);
	return new_ptr;
}

void free(void *ptr)
{
	struct block *blk, *curr;

	if (!ptr)
		return;

	blk = ((struct block *)(ptr)) - 1;
	blk->free = true;

	curr = head;
	while (curr && curr->next) {
		if (curr->free && curr->next->free) {
			curr->size += sizeof(struct block);
			curr->next = curr->next->next;
		}
		curr = curr->next;
	}
}

int wait(int *wstatus)
{
	return wait4(-1, wstatus, 0, 0);
}

int waitpid(pid_t pid, int *wstatus, int options)
{
	return wait4(pid, wstatus, options, 0);
}

static char *environ[] = { NULL };

int execv(const char *path, char *const argv[])
{
	return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
	return execve(file, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
	return execve(file, argv, envp);
}
