#include "ulib.h"

static char buf[1024];

int cat(int fd)
{
	ssize_t rcnt;

	while ((rcnt = read(fd, buf, 1024)) > 0) {
		if (write(STDOUT_FILENO, buf, rcnt) != rcnt) {
			dprintf(STDERR_FILENO, "cat: write error\n");
			return 1;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int fd;

	if (argc <= 1)
		return cat(STDIN_FILENO);

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		dprintf(STDERR_FILENO, "cat: open %s failed: %s\n", argv[1],
			strerror(fd));
		return 1;
	}

	int ret = cat(fd);
	close(fd);

	return ret;
}
