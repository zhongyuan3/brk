#include "ulib.h"

static struct dirent64 buf[3];

int main(int argc, char *argv[])
{
	char *path = ".";

	if (argc > 1)
		path = argv[1];

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("ls: open %s failed\n", path);
		return 1;
	}

	ssize_t rcnt;

	while ((rcnt = getdents64(fd, buf, sizeof(buf))) > 0) {
		ssize_t i = 0;
		struct dirent64 *p = buf;
		while (i < rcnt) {
			printf("%s\n", p->d_name);
			i += p->d_reclen;
			p = (struct dirent64 *)((uint64_t)p + p->d_reclen);
		}
	}

	if (rcnt < 0) {
		close(fd);
		printf("ls: getdents64 failed: %s\n", strerror(rcnt));
		return 1;
	}

	close(fd);

	return 0;
}
