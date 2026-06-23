#ifndef ARCH_SBI_H
#define ARCH_SBI_H

#include <brk/types.h>

#define SBI_LEGACY_SET_TIMER 0x00
#define SBI_LEGACY_CONSOLE_PUTCHAR 0x01
#define SBI_LEGACY_CONSOLE_GETCHAR 0x02
#define SBI_LEGACY_SHUTDOWN 0x08

#define SBI_EXT_HSM 0x48534D
#define SBI_EXT_HSM_HART_START 0x00
#define SBI_EXT_HSM_HART_STOP 0x01
#define SBI_EXT_HSM_HART_GET_STATUS 0x02

#define SBI_SUCCESS 0
#define SBI_ERR_FAILURE -1
#define SBI_ERR_NOT_SUPPORTED -2
#define SBI_ERR_INVALID_PARAM -3
#define SBI_ERR_DENIED -4
#define SBI_ERR_INVALID_ADDRESS -5
#define SBI_ERR_ALREADY_AVAILABLE -6

struct sbiret {
	long error;
	long value;
};

static inline struct sbiret sbi_ecall(unsigned long eid, unsigned long fid,
				      unsigned long arg0, unsigned long arg1,
				      unsigned long arg2, unsigned long arg3,
				      unsigned long arg4, unsigned long arg5)
{
	struct sbiret ret;

	register unsigned long a7 asm("a7") = eid;
	register unsigned long a6 asm("a6") = fid;
	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1") = arg1;
	register unsigned long a2 asm("a2") = arg2;
	register unsigned long a3 asm("a3") = arg3;
	register unsigned long a4 asm("a4") = arg4;
	register unsigned long a5 asm("a5") = arg5;

	asm volatile("ecall"
		     : "+r"(a0), "+r"(a1)
		     : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
		     : "memory");

	ret.error = a0;
	ret.value = a1;

	return ret;
}

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

static inline struct sbiret sbi_hart_start(unsigned long hartid,
					   unsigned long start_addr,
					   unsigned long opaque)
{
	return sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START, hartid,
			 start_addr, opaque, 0, 0, 0);
}

#endif
