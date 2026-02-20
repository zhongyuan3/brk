#ifndef AOSD_RISCV_H
#define AOSD_RISCV_H

#include <aosd/types.h>

static inline uint64_t read_tp(void)
{
	uint64_t x;
	asm volatile("mv %0, tp" : "=r"(x));
	return x;
}

static inline void write_tp(uint64_t x)
{
	asm volatile("mv tp, %0" : : "r"(x));
}

#define SSTATUS_SPP (1 << 8) /* Previous mode, 1=Supervisor, 0=User */
#define SSTATUS_SPIE (1 << 5) /* Supervisor Previous Interrupt Enable */
#define SSTATUS_UPIE (1 << 4) /* User Previous Interrupt Enable */
#define SSTATUS_SIE (1 << 1) /* Supervisor Interrupt Enable */
#define SSTATUS_UIE (1 << 0) /* User Interrupt Enable */

static inline uint64_t read_sstatus(void)
{
	uint64_t x;
	asm volatile("csrr %0, sstatus" : "=r"(x));
	return x;
}

static inline void write_sstatus(uint64_t x)
{
	asm volatile("csrw sstatus, %0" : : "r"(x));
}

static inline uint64_t read_satp(void)
{
	uint64_t x;
	asm volatile("csrr %0, satp" : "=r"(x));
	return x;
}

static inline uint64_t make_satp_sv39(uint64_t pt)
{
	return ((uint64_t)8 << 60) | (pt >> 12);
}

static inline void write_satp(uint64_t x)
{
	asm volatile("csrw satp, %0" : : "r"(x));
}

static inline void sfence_vma(void)
{
	asm volatile("sfence.vma zero, zero");
}

static inline uint64_t read_stvec(void)
{
	uint64_t x;
	asm volatile("csrr %0, stvec" : "=r"(x));
	return x;
}

static inline void write_stvec(uint64_t x)
{
	asm volatile("csrw stvec, %0" : : "r"(x));
}

static inline uint64_t read_scause(void)
{
	uint64_t x;
	asm volatile("csrr %0, scause" : "=r"(x));
	return x;
}

static inline uint64_t read_sepc(void)
{
	uint64_t x;
	asm volatile("csrr %0, sepc" : "=r"(x));
	return x;
}

static inline void write_sepc(uint64_t x)
{
	asm volatile("csrw sepc, %0" : : "r"(x));
}

static inline uint64_t read_stval(void)
{
	uint64_t x;
	asm volatile("csrr %0, stval" : "=r"(x));
	return x;
}

#define SIE_SEIE (1L << 9) /* External */
#define SIE_STIE (1L << 5) /* Timer */
#define SIE_SSIE (1L << 1) /* Software */

static inline uint64_t read_sie(void)
{
	uint64_t x;
	asm volatile("csrr %0, sie" : "=r"(x));
	return x;
}

static inline void write_sie(uint64_t x)
{
	asm volatile("csrw sie, %0" : : "r"(x));
}

static inline uint64_t read_time(void)
{
	uint64_t time;
	asm volatile("csrr %0, time" : "=r"(time));
	return time;
}

static inline void intr_on(void)
{
	write_sstatus(read_sstatus() | SSTATUS_SIE);
}

static inline void intr_off(void)
{
	write_sstatus(read_sstatus() & ~SSTATUS_SIE);
}

static inline bool intr_enabled(void)
{
	return read_sstatus() & SSTATUS_SIE;
}

static inline uint64_t read_sscratch(void)
{
	uint64_t x;
	asm volatile("csrr %0, sscratch" : "=r"(x));
	return x;
}

static inline void write_sscratch(uint64_t x)
{
	asm volatile("csrw sscratch, %0" : : "r"(x));
}

#endif
