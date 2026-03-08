#include "ulib.h"

#define BUF_SIZE 4096

static char buf[BUF_SIZE];
static char line_buf[BUF_SIZE];

static int grep(int fd, const char *pattern)
{
	int line_pos = 0;
	ssize_t rcnt;
	int line_number = 1;
	int found = 0;

	while ((rcnt = read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < rcnt; i++) {
			if (buf[i] == '\n') {
				line_buf[line_pos] = '\0';

				if (strstr(line_buf, pattern)) {
					printf("%d:%s\n", line_number,
					       line_buf);
					found = 1;
				}

				line_pos = 0;
				line_number++;
			} else {
				if (line_pos < BUF_SIZE - 1) {
					line_buf[line_pos++] = buf[i];
				}
			}
		}
	}

	if (line_pos > 0) {
		line_buf[line_pos] = '\0';
		if (strstr(line_buf, pattern)) {
			printf("%d:%s\n", line_number, line_buf);
			found = 1;
		}
	}

	if (!found)
		return 1;

	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("Usage: grep PATTERN [FILE]\n");
		return 1;
	}

	if (argc < 3)
		return grep(STDIN_FILENO, argv[1]);

	int fd = open(argv[2], O_RDONLY);
	if (fd < 0) {
		dprintf(STDERR_FILENO, "open %s failed: %s\n", argv[2],
			strerror(fd));
		return 1;
	}
	int ret = grep(fd, argv[1]);
	close(fd);
	return ret;
}
