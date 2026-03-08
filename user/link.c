#include "ulib.h"

int main(int argc, char *argv[])
{
	if (argc != 3) {
		dprintf(STDERR_FILENO, "Usage: link <target> <linkname>\n");
		return 1;
	}

	int err = link(argv[1], argv[2]);
	if (err) {
		dprintf(STDERR_FILENO, "link: %s failed to create: %s\n",
			argv[2], strerror(err));
		return 1;
	}

	return 0;
}
