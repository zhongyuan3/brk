#include "ulib.h"

char buf[512];

int wc(int fd, char *name)
{
	size_t line_cnt = 0;
	size_t word_cnt = 0;
	size_t char_cnt = 0;
	bool word = false;
	while (1) {
		ssize_t rcnt = read(fd, buf, sizeof(buf));
		if (rcnt < 0) {
			perror("wc: read error");
			return 1;
		}
		if (rcnt < 1)
			break;
		for (ssize_t i = 0; i < rcnt; i++) {
			char_cnt++;
			if (buf[i] == '\n')
				line_cnt++;
			if (strchr(" \r\t\n\v", buf[i]))
				word = false;
			else if (!word) {
				word_cnt++;
				word = true;
			}
		}
	}

	dprintf(STDOUT_FILENO, "%lu %lu %lu %s\n", line_cnt, word_cnt, char_cnt,
		name);
	return 0;
}

int main(int argc, char *argv[])
{
	int fd, i;

	if (argc <= 1) {
		wc(0, "");
		return 0;
	}

	for (i = 1; i < argc; i++) {
		fd = open(argv[i], O_RDONLY);
		if (fd < 0) {
			perror("wc: open failed");
			return 1;
		}
		int ret = wc(fd, argv[i]);
		close(fd);
		if (ret != 0)
			return 1;
	}

	return 0;
}
