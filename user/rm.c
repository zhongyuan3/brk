#include "ulib.h"

int main(int argc, char *argv[])
{
	if (argc < 2) {
		dprintf(STDERR_FILENO, "usage: rm files...\n");
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		int err = unlink(argv[i]);
		if (err) {
			dprintf(STDERR_FILENO, "rm: %s failed to delete: %s\n",
				argv[i], strerror(err));
			return 1;
		}
	}

	return 0;
}
