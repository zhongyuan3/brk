#include "ulib.h"

int main(int argc, char *argv[])
{
	if (argc < 2) {
		dprintf(STDERR_FILENO, "Usage: mkdir files...\n");
		return 1;
	}

	for (int i = 1; i < argc; ++i) {
		int err = mkdir(argv[i], 0);
		if (err) {
			perror("mkdir");
			return 1;
		}
	}

	return 0;
}
