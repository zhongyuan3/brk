#include "aosd/types.h"
#include "ulib.h"
#include <stddef.h>

int main(void)
{
	write(STDOUT_FILENO, "welcome to aosd v0.1.0\n", 23);

	while (1) {
		pid_t cpid = fork();
		if (cpid < 0) {
			perror("fork failed");
			goto end;
		}

		if (cpid == 0) {
			char *argv[] = { "/bin/sh", 0 };
			char *envp[] = { 0 };
			execve(argv[0], argv, envp);
			perror("execve failed");
			exit(1);
		}

		while (1) {
			pid_t wpid = wait(0);
			if (wpid == cpid)
				break;
		}
	}

end:
	write(STDERR_FILENO, "failed to start shell\n", 22);
	while (1)
		wait(0);
}
