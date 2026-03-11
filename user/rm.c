#include "ulib.h"

int main(int argc, char *argv[])
{
	if (argc < 2) {
		dprintf(STDERR_FILENO, "Usage: rm files...\n");
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		int err = unlink(argv[i]);
		if (err) {
			perror("rm");
			return 1;
		}
	}

	return 0;
}
