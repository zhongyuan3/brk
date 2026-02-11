#ifndef AOSD_SBI_H
#define AOSD_SBI_H

#include <aosd/types.h>

#define SBI_LEGACY_SET_TIMER 0x00
#define SBI_LEGACY_CONSOLE_PUTCHAR 0x01
#define SBI_LEGACY_CONSOLE_GETCHAR 0x02
#define SBI_LEGACY_SHUTDOWN 0x08

static inline long sbi_set_timer(uint64_t stime_value)
{
	register uint64_t a0 asm("a0") = stime_value;
	register uint64_t a7 asm("a7") = SBI_LEGACY_SET_TIMER;
	asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
	return a0;
}

static inline long sbi_console_putchar(int ch)
{
	register uint64_t a0 asm("a0") = (uint64_t)(unsigned char)ch;
	register uint64_t a7 asm("a7") = SBI_LEGACY_CONSOLE_PUTCHAR;
	asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
	return a0;
}

static inline long sbi_console_getchar(void)
{
	long ch;
	register uint64_t a7 asm("a7") = SBI_LEGACY_CONSOLE_GETCHAR;
	asm volatile("ecall" : "=r"(ch) : "r"(a7) : "memory");
	return ch;
}

static inline void sbi_shutdown(void)
{
	register uint64_t a7 asm("a7") = SBI_LEGACY_SHUTDOWN;
	asm volatile("ecall" : : "r"(a7) : "memory");
}

static inline void sbi_console_putstr(char const *s)
{
	while (*s)
		sbi_console_putchar(*s++);
}

#endif
