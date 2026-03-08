#include "ulib.h"
#include "internal.h"

int errno = 0;
static char *environ[] = { NULL };
static char *sys_path[] = { "/bin" };

void _start(int argc, char **argv, char **envp)
{
	extern int main(int argc, char **argv, char **envp);
	exit(main(argc, argv, envp));
}

int wait(int *wstatus)
{
	return wait4(-1, wstatus, 0, 0);
}

int waitpid(pid_t pid, int *wstatus, int options)
{
	return wait4(pid, wstatus, options, 0);
}

int execv(const char *path, char *const argv[])
{
	return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
	return execvpe(file, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
	char path_buf[PATH_MAX];

	execve(file, argv, envp);

	size_t file_len = strlen(file);
	if (file_len >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}

	for (size_t i = 0; i < sizeof(sys_path) / sizeof(sys_path[0]); i++) {
		size_t sys_path_len = strlen(sys_path[i]);
		if (sys_path_len + file_len + 2 >= PATH_MAX) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(path_buf, sys_path[i], sys_path_len);
		path_buf[sys_path_len] = '/';
		memcpy(path_buf + sys_path_len + 1, file, file_len);
		path_buf[sys_path_len + file_len + 1] = '\0';
		execve(path_buf, argv, envp);
	}

	return -1;
}

void *sbrk(intptr_t increment)
{
	static uint64_t curr_brk = 0;
	if (curr_brk == 0)
		curr_brk = syscall(SYS_brk, 0);
	uint64_t new_brk = (uint64_t)((intptr_t)curr_brk + increment);
	int ret = syscall(SYS_brk, new_brk);
	if (ret != 0) {
		errno = -ret;
		return (void *)-1;
	}
	uint64_t old_brk = curr_brk;
	curr_brk = new_brk;
	errno = 0;
	return (void *)old_brk;
}
