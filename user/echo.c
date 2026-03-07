#include "ulib.h"

int main(int argc, char *argv[])
{
	if (argc > 1)
		write(STDOUT_FILENO, argv[1], strlen(argv[1]));

	for (int i = 2; i < argc; ++i) {
		write(STDOUT_FILENO, " ", 1);
		write(STDOUT_FILENO, argv[1], strlen(argv[1]));
	}

	write(STDOUT_FILENO, "\n", 1);

	return 0;
}
