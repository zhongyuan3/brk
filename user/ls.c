#include "ulib.h"

struct ls_option {
	const char *path;
	bool long_format;
};

static struct dirent64 buf[30];

static void print_dirent_long(struct dirent64 *p)
{
	const char *type = "?";
	switch (p->d_type) {
	case DT_FIFO:
		type = "p";
		break;
	case DT_CHR:
		type = "c";
		break;
	case DT_DIR:
		type = "d";
		break;
	case DT_BLK:
		type = "b";
		break;
	case DT_REG:
		type = "-";
		break;
	case DT_LNK:
		type = "l";
		break;
	case DT_SOCK:
		type = "s";
		break;
	case DT_WHT:
		type = "w";
		break;
	}
	dprintf(STDOUT_FILENO, "%s %s%s\n", type, p->d_name,
		p->d_type == 4 ? "/" : "");
}

static int ls(int fd, const struct ls_option *opt)
{
	ssize_t rcnt;

	while (1) {
		rcnt = getdents64(fd, buf, sizeof(buf));
		if (rcnt < 0) {
			perror("ls: getdents64 failed");
			return 1;
		}
		if (rcnt < 1)
			break;
		ssize_t i = 0;
		struct dirent64 *p = buf;
		while (i < rcnt) {
			if (opt->long_format)
				print_dirent_long(p);
			else
				dprintf(STDOUT_FILENO, "%s\n", p->d_name);
			i += p->d_reclen;
			p = (struct dirent64 *)((uint64_t)p + p->d_reclen);
		}
	}

	return 0;
}

static int parse_config(int argc, char *argv[], struct ls_option *opt)
{
	for (int i = 1; i < argc; ++i) {
		if (argv[i][0] == '-') {
			if (!strcmp(argv[i], "--long") ||
			    !strcmp(argv[i], "-l")) {
				opt->long_format = true;
			} else {
				dprintf(STDERR_FILENO,
					"ls: unknown option %s\n", argv[i]);
				return 1;
			}
		} else {
			opt->path = argv[i];
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct ls_option opt = { .path = "." };

	int ret = parse_config(argc, argv, &opt);
	if (ret != 0) {
		return 1;
	}

	int fd = open(opt.path, O_RDONLY);
	if (fd < 0) {
		perror("ls: open failed");
		return 1;
	}

	ret = ls(fd, &opt);
	close(fd);

	return ret;
}
