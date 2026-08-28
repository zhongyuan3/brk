#ifndef ARCH_CSR_H
#define ARCH_CSR_H

#include <brk/base/types.h>

/*
 * XLEN-wide CSRs are read/written through unsigned long: on RV32 these
 * CSRs are 32 bits wide, and using uint64_t there would let GCC allocate
 * an uninitialized second register for the high half.
 */

static inline uintptr_t read_tp(void)
{
	uintptr_t x;
	asm volatile("mv %0, tp" : "=r"(x));
	return x;
}

static inline void write_tp(uintptr_t x)
{
	asm volatile("mv tp, %0" : : "r"(x));
}

#define SSTATUS_SPP (1 << 8) /* Previous mode, 1=Supervisor, 0=User */
#define SSTATUS_SPIE (1 << 5) /* Supervisor Previous Interrupt Enable */
#define SSTATUS_UPIE (1 << 4) /* User Previous Interrupt Enable */
#define SSTATUS_SIE (1 << 1) /* Supervisor Interrupt Enable */
#define SSTATUS_UIE (1 << 0) /* User Interrupt Enable */
#define SSTATUS_SUM (1 << 18) /* Permit Supervisor User Memory access */

static inline unsigned long read_sstatus(void)
{
	unsigned long x;
	asm volatile("csrr %0, sstatus" : "=r"(x));
	return x;
}

static inline void write_sstatus(unsigned long x)
{
	asm volatile("csrw sstatus, %0" : : "r"(x));
}

static inline unsigned long read_satp(void)
{
	unsigned long x;
	asm volatile("csrr %0, satp" : "=r"(x));
	return x;
}

static inline uint64_t make_satp(uint64_t pt)
{
#if __riscv_xlen == 32
	/* Sv32: mode 1 (bits 31:30), 22-bit PPN (bits 21:0). */
	return (1U << 31) | (pt >> 12);
#else
	/* Sv39: mode 8 (bits 63:60), 44-bit PPN (bits 43:0). */
	return ((uint64_t)8 << 60) | (pt >> 12);
#endif
}

static inline void write_satp(unsigned long x)
{
	asm volatile("csrw satp, %0" : : "r"(x));
}

static inline void sfence_vma(void)
{
	asm volatile("sfence.vma zero, zero");
}

static inline void fence_i(void)
{
	asm volatile("fence.i" ::: "memory");
}

static inline unsigned long read_stvec(void)
{
	unsigned long x;
	asm volatile("csrr %0, stvec" : "=r"(x));
	return x;
}

static inline void write_stvec(unsigned long x)
{
	asm volatile("csrw stvec, %0" : : "r"(x));
}

static inline unsigned long read_scause(void)
{
	unsigned long x;
	asm volatile("csrr %0, scause" : "=r"(x));
	return x;
}

static inline unsigned long read_sepc(void)
{
	unsigned long x;
	asm volatile("csrr %0, sepc" : "=r"(x));
	return x;
}

static inline void write_sepc(unsigned long x)
{
	asm volatile("csrw sepc, %0" : : "r"(x));
}

static inline unsigned long read_stval(void)
{
	unsigned long x;
	asm volatile("csrr %0, stval" : "=r"(x));
	return x;
}

#define SIE_SEIE (1L << 9) /* External */
#define SIE_STIE (1L << 5) /* Timer */
#define SIE_SSIE (1L << 1) /* Software */

#define SIP_SSIP (1L << 1) /* Supervisor software interrupt pending */

static inline unsigned long read_sie(void)
{
	unsigned long x;
	asm volatile("csrr %0, sie" : "=r"(x));
	return x;
}

static inline void write_sie(unsigned long x)
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

/**
 * @brief Atomically disable the interrupts and return the previous state
 *
 * @return true if interrupts were enabled
 * @return false if interrupts were disabled
 */
static inline bool intr_off_get(void)
{
	unsigned long old;
	unsigned long i = SSTATUS_SIE;
	asm volatile("csrrc %0, sstatus, %1" : "=r"(old) : "r"(i));
	return (old & SSTATUS_SIE) != 0;
}

static inline unsigned long read_sscratch(void)
{
	unsigned long x;
	asm volatile("csrr %0, sscratch" : "=r"(x));
	return x;
}

static inline void write_sscratch(unsigned long x)
{
	asm volatile("csrw sscratch, %0" : : "r"(x));
}

static inline void clear_ssip_csr(void)
{
	asm volatile("csrc sip, %0" : : "r"((unsigned long)SIP_SSIP));
}

#endif