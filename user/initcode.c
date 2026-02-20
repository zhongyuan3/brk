void putchar_(int c);

void _start(void)
{
	volatile long x = 0;
	while (1) {
		if (x % 50000000 == 0)
			putchar_('A');
		++x;
	}
}

void putchar_(int c)
{
	asm volatile("ecall");
}
