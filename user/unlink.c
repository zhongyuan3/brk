#include "ulib.h"

int main(int argc, char *argv[])
{
	if (argc != 2) {
		dprintf(STDERR_FILENO, "Usage: unlink <linkname>\n");
		return 1;
	}

	int err = unlink(argv[1]);
	if (err) {
		dprintf(STDERR_FILENO, "unlink: %s failed: %s\n", argv[1],
			strerror(err));
		return 1;
	}

	return 0;
}
