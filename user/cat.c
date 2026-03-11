#include "ulib.h"

static char buf[1024];

int cat(int fd)
{
	ssize_t rcnt, wcnt;

	while (1) {
		rcnt = read(fd, buf, 1024);
		if (rcnt < 0) {
			perror("cat: read error");
			return 1;
		}
		if (rcnt < 1)
			break;
		wcnt = write(STDOUT_FILENO, buf, rcnt);
		if (wcnt < 0) {
			perror("cat: write error");
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
		perror("cat: open failed");
		return 1;
	}

	int ret = cat(fd);
	close(fd);

	return ret;
}
